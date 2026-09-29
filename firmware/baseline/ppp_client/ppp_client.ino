/* TASK-020 stage 6: real PPP client over the modem UART.
 *
 * The earlier probe established two facts but stopped at the modem's "CONNECT":
 *   - the Airtel SIM does assign a real data IP
 *   - ATD*99# makes the A7670E enter PPP data mode
 * Neither one says that LCP and IPCP actually negotiate, or that the ESP32 ends
 * up with a usable IP interface. That is the precondition for NAT forwarding, so
 * this sketch runs an actual PPP stack and reports what happens.
 *
 * WHY NOT THE ARDUINO PPP LIBRARY
 * The bundled PPP library drives the link through esp_modem, and esp_modem's
 * A7670 support is USB-only. This board wires the modem to GPIO17/18. Rather
 * than fight that mismatch, this talks to lwIP's PPP stack directly: the core
 * already ships it compiled in (CONFIG_LWIP_PPP_SUPPORT=y) with the headers and
 * the thread-safe pppapi_ wrappers, so the only thing missing is a UART-backed
 * PPPoS session. That is about 100 lines.
 *
 * The flow is: bring the modem up, confirm it is registered, ask for data on
 * the APN, ATD*99#, and on CONNECT hand the UART to PPP. From then on the bytes
 * are PPP frames, not AT commands, and no AT is sent again.
 *
 * Two rules learned the hard way in BUG-005 are enforced here:
 *   1. A PWRKEY pulse toggles power on an off modem and hangs up on a live one.
 *      It is not a reset, so it is only sent after probing proves silence.
 *   2. Once the modem is in data mode we must never cut its power, and we must
 *      not send AT commands, because those bytes would be interpreted as PPP.
 */

#include <Arduino.h>
#include <lwip/init.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>
#include <netif/ppp/pppos.h>
#include <netif/ppp/ppp.h>
/* ppp_impl.h is nominally private, but it is shipped with the core and is the
 * only way to reach ppp_recv_config(). See the MRU comment at the call site
 * for why that is required. */
#include <netif/ppp/ppp_impl.h>

#define MODEM_TX_PIN  18
#define MODEM_RX_PIN  17
#define MODEM_PWR_PIN 21
#define MODEM_PWRKEY  42
#define MODEM_BAUD    115200

#define PPP_MTU       1500
#define APN           "airtelgprs.com"
#define SETTLE_MS     1200
#define DIAL_TIMEOUT  25000
#define NEGOTIATE_MS  45000

HardwareSerial ss(1);

static struct netif ppp_netif;
static ppp_pcb *g_pcb = NULL;

/* Set from the tcpip thread, read and printed from loop(). Printing directly
 * inside a PPP callback would block the network stack on a slow USB CDC write,
 * so the callbacks only latch state. */
static volatile uint8_t  g_last_phase  = 0xFF;
static volatile int       g_link_err    = -1;
static volatile bool      g_got_ip      = false;
static volatile uint32_t g_tx_bytes    = 0;
static volatile uint32_t g_tx_frames   = 0;
static volatile uint32_t g_rx_bytes    = 0;
static volatile bool      g_dialed      = false;

/* Drain up to len bytes from the modem UART, waiting at most waitMs total.
 * readBytes() has no timed buffer overload, so this polls available(). */
static size_t drainUart(uint8_t *buf, size_t len, unsigned long waitMs) {
  unsigned long t0 = millis();
  size_t n = 0;
  while (n < len && millis() - t0 < waitMs) {
    int avail = ss.available();
    if (avail > 0) {
      int chunk = avail < (int)(len - n) ? avail : (int)(len - n);
      for (int i = 0; i < chunk; i++) buf[n + i] = (uint8_t)ss.read();
      n += (size_t)chunk;
    } else {
      yield();
    }
  }
  return n;
}

static const char *phaseName(uint8_t p) {
  switch (p) {
    case PPP_PHASE_DEAD:         return "DEAD";
    case PPP_PHASE_MASTER:       return "MASTER";
    case PPP_PHASE_HOLDOFF:      return "HOLDOFF";
    case PPP_PHASE_INITIALIZE:   return "INITIALIZE";
    case PPP_PHASE_SERIALCONN:   return "SERIALCONN";
    case PPP_PHASE_DORMANT:      return "DORMANT";
    case PPP_PHASE_ESTABLISH:    return "ESTABLISH";
    case PPP_PHASE_AUTHENTICATE: return "AUTHENTICATE";
    case PPP_PHASE_CALLBACK:     return "CALLBACK";
    case PPP_PHASE_NETWORK:      return "NETWORK";
    case PPP_PHASE_RUNNING:      return "RUNNING";
    case PPP_PHASE_TERMINATE:    return "TERMINATE";
    case PPP_PHASE_DISCONNECT:   return "DISCONNECT";
    default:                     return "UNKNOWN";
  }
}

/* --- AT helpers, only valid in command mode ------------------------- */

static String sendAT(const char *cmd, unsigned long timeout) {
  /* Drain anything left from a previous command first. Otherwise a stale
   * "OK" from the last exchange is read as this command's reply, and the
   * output ends up with each label paired with the previous command's answer.
   * That is exactly the bug that made the first ppp_probe verdict wrong. */
  ss.flush();
  while (ss.available()) ss.read();
  delay(120);

  ss.print(cmd); ss.print("\r\n");
  unsigned long t0 = millis();
  String got = "";
  while (millis() - t0 < timeout) {
    while (ss.available()) got += (char)ss.read();
    if (got.indexOf("\r\nOK") != -1 || got.indexOf("\r\nERROR") != -1 ||
        got.indexOf("+CME ERROR") != -1) break;
    yield();
  }
  /* Collect the trailing status line so nothing is left dangling. */
  delay(60);
  while (ss.available()) got += (char)ss.read();
  return got;
}

static bool modemAnswers(unsigned long perTry, int tries) {
  for (int i = 0; i < tries; i++) {
    if (sendAT("AT", perTry).indexOf("OK") != -1) return true;
    delay(400);
  }
  return false;
}

/* Escape the modem out of PPP data mode.
 *
 * A modem in data mode has no AT parser, so every AT is silently discarded.
 * That silence is indistinguishable from a dead modem, which is what made
 * BUG-005 look like a hard power fault. It is not: the module is alive and
 * holding an open data session. The standard way out is the "+++" guard
 * sequence, which requires silence on the line either side of it, so the UART
 * must be quiet before and after.
 *
 * Returns true if the modem answered AT afterwards.
 */
static bool escapeDataMode() {
  ss.flush();
  while (ss.available()) ss.read();
  delay(1100);                    /* guard time before */
  ss.print("+++");
  ss.flush();
  delay(1100);                    /* guard time after */

  if (modemAnswers(2000, 3)) {
    Serial0.println("  '+++' escape accepted - modem was in data mode");
    Serial0.flush();
    return true;
  }
  return false;
}

static void logLine(const char *label, const char *cmd) {
  String r = sendAT(cmd, 2000);
  Serial0.printf("  %-22s %s\n", label, r.length() ? r.c_str() : "(no reply)");
  Serial0.flush();
}

static void powerOnModem() {
  pinMode(MODEM_PWR_PIN, OUTPUT);
  digitalWrite(MODEM_PWR_PIN, HIGH);
  delay(800);

  ss.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  delay(400);

  /* 1. Already in command mode? Nothing to do. */
  Serial0.println("PWR-ON: probing before any PWRKEY pulse");
  Serial0.flush();
  if (modemAnswers(1500, 3)) {
    Serial0.println("PWR-ON: modem already in command mode");
    Serial0.flush();
    return;
  }

  /* 2. Silence that is really a stuck data session. Try the escape sequence
   *    before touching power, because power-cycling mid-session is what
   *    created this whole class of problem in the first place. */
  if (escapeDataMode()) return;

  /* 3. Genuinely off: only now is a PWRKEY pulse meaningful. On an already-on
   *    modem a pulse hangs up, so it must never be sent speculatively. */
  Serial0.println("PWR-ON: silent after escape, sending PWRKEY pulse");
  Serial0.flush();
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, HIGH); delay(100);
  digitalWrite(MODEM_PWRKEY, LOW);  delay(1200);
  digitalWrite(MODEM_PWRKEY, HIGH); delay(6000);
  Serial0.println(modemAnswers(2000, 8) ? "PWR-ON: up after PWRKEY"
                                       : "PWR-ON: STILL SILENT");
  Serial0.flush();
}

/* --- PPP callbacks --------------------------------------------------- */

static u32_t pppOut(ppp_pcb *pcb, const void *data, u32_t len, void *ctx) {
  (void)pcb; (void)ctx;
  const uint8_t *b = (const uint8_t *)data;
  u32_t sent = 0;
  while (sent < len) {
    size_t n = ss.write(b + sent, len - sent);
    if (n == 0) { delayMicroseconds(200); continue; }
    sent += (u32_t)n;
  }
  ss.flush();
  g_tx_bytes += sent;
  g_tx_frames++;

  /* Hex-log the first few frames. A PPP frame that is malformed or missing its
   * 0x7E flags will be silently ignored by the modem, and "we sent bytes but
   * got nothing back" is otherwise impossible to tell apart from a dead link.
   * Logging the actual wire bytes is the only way to tell those apart. */
  if (g_tx_frames <= 3) {
    Serial0.printf("  TX frame %lu (%u bytes):", (unsigned long)g_tx_frames, (unsigned)len);
    for (u32_t i = 0; i < len && i < 48; i++) Serial0.printf(" %02X", b[i]);
    if (len > 48) Serial0.print(" ...");
    Serial0.println();
    Serial0.flush();
  }
  return sent;
}

static void pppPhase(ppp_pcb *pcb, u8_t phase, void *ctx) {
  (void)pcb; (void)ctx;
  g_last_phase = phase;
  if (phase == PPP_PHASE_RUNNING) g_got_ip = true;
}

static void pppLink(ppp_pcb *pcb, int err_code, void *ctx) {
  (void)pcb; (void)ctx;
  g_link_err = err_code;
}

static void showIp(const char *when) {
  const ip4_addr_t *a = netif_ip4_addr(&ppp_netif);
  const ip4_addr_t *g = netif_ip4_gw(&ppp_netif);
  char abuf[20], gbuf[20];
  strlcpy(abuf, ip4addr_ntoa(a), sizeof(abuf));
  strlcpy(gbuf, ip4addr_ntoa(g), sizeof(gbuf));
  Serial0.printf("  %-10s local=%s  gateway=%s  up=%d  mtu=%d\n",
                 when, abuf, gbuf, (int)netif_is_up(&ppp_netif), ppp_netif.mtu);
  Serial0.flush();
}

/* CRC-16/X-25 exactly as PPP uses it: reflected poly 0x8408, init 0xFFFF and a
 * final XOR of 0xFFFF. */
static uint16_t pppFcs16(const uint8_t *d, size_t n) {
  uint16_t fcs = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    fcs ^= d[i];
    for (int b = 0; b < 8; b++)
      fcs = (fcs & 1) ? (uint16_t)((fcs >> 1) ^ 0x8408) : (uint16_t)(fcs >> 1);
  }
  return (uint16_t)(fcs ^ 0xFFFF);
}

static void writeEscaped(uint8_t b) {
  if (b < 0x20 || b == 0x7D || b == 0x7E) {
    ss.write((uint8_t)0x7D);
    ss.write((uint8_t)(b ^ 0x20));
  } else {
    ss.write(b);
  }
}

/* Frame note: neither lwIP's Configure-Request nor the A7670E's own replies
 * carry the 2-byte RFC 1662 length field, so both framings are sent and the
 * answer decides which one this modem actually wants. */
static size_t buildLcpFrame(uint8_t *out, uint8_t id, uint16_t mru, bool withLength) {
  uint8_t body[80];
  size_t n = 0;
  body[n++] = 0xFF;
  body[n++] = 0x03;
  body[n++] = 0xC0;  /* LCP */
  body[n++] = 0x21;
  if (withLength) {
    body[n++] = 0x00;
    body[n++] = 0x18;  /* protocol(2) + length(2) + LCP(20) */
  }
  body[n++] = 0x01;                    /* Configure-Request */
  body[n++] = id;
  body[n++] = 0x00;
  body[n++] = 0x14;                    /* LCP length = 20 */
  body[n++] = 0x02; body[n++] = 0x06;  /* MRU */
  body[n++] = (uint8_t)(mru >> 8);
  body[n++] = (uint8_t)(mru & 0xFF);
  body[n++] = 0x00; body[n++] = 0x00;
  body[n++] = 0x05; body[n++] = 0x06;  /* Magic-Number */
  body[n++] = 0x11; body[n++] = 0x22; body[n++] = 0x33; body[n++] = 0x44;
  body[n++] = 0x07; body[n++] = 0x02;  /* Protocol-Field-Compression */
  body[n++] = 0x08; body[n++] = 0x02;  /* Addr/Control-Field-Compression */

  uint16_t fcs = pppFcs16(body, n);
  size_t o = 0;
  out[o++] = 0x7E;
  for (size_t i = 0; i < n; i++) out[o++] = body[i];
  /* RFC 1662 puts the FCS on the wire least-significant byte first. Verified
   * against the A7670E's own Terminate-Ack, whose FCS 0x940D appears on the
   * wire as 94 0D. Sending it big-endian makes the peer silently drop every
   * frame, which looks exactly like a dead link. */
  out[o++] = (uint8_t)(fcs & 0xFF);
  out[o++] = (uint8_t)(fcs >> 8);
  out[o++] = 0x7E;
  return o;
}

static void sendRawFrame(const uint8_t *f, size_t n) {
  ss.write((uint8_t)0x7E);
  for (size_t i = 1; i + 1 < n; i++) writeEscaped(f[i]);
  ss.write((uint8_t)0x7E);
  ss.flush();
}

static void rawTrial(const char *label, uint8_t id, uint16_t mru, bool withLength) {
  uint8_t frame[96];
  size_t n = buildLcpFrame(frame, id, mru, withLength);
  Serial0.printf("  %-34s", label);
  Serial0.flush();
  sendRawFrame(frame, n);

  uint8_t buf[128];
  size_t got = drainUart(buf, sizeof(buf), 4000);
  if (got == 0) {
    Serial0.println("silence");
  } else {
    Serial0.printf("REPLY (%u bytes):", (unsigned)got);
    for (size_t i = 0; i < got && i < 48; i++) Serial0.printf(" %02X", buf[i]);
    Serial0.println();
  }
  Serial0.flush();
  delay(400);
}

void setup() {
  Serial0.begin(115200);
  Serial0.println("\n========== TASK-020 STAGE 6: PPP CLIENT ==========");
  Serial0.flush();
  delay(1500);

  powerOnModem();
  if (!modemAnswers(2000, 3)) {
    Serial0.println("\nRESULT PPP_NEGOTIATION FAIL - modem unreachable");
    Serial0.println("  This is BUG-005. Unplug USB, wait 10s, re-plug, re-run.");
    Serial0.flush();
    return;
  }
  Serial0.println("MODEM-AT-OK\n");
  Serial0.flush();

  /* --- command mode setup --- */
  logLine("echo off",           "ATE0");
  logLine("firmware",           "AT+CGMR");
  logLine("network search",     "AT+COPS?");
  logLine("signal",             "AT+CSQ");
  logLine("EPS registration",   "AT+CEREG?");
  logLine("CS registration",    "AT+CREG?");
  logLine("attach",             "AT+CGATT=1");
  logLine("set APN",            "AT+CGDCONT=1,\"IP\",\"" APN "\"");

  /* Clear any session left behind by a previous run before asking for a new
   * one. After the earlier crash the module still had a data call up, and
   * ATD*99# answered "+CME ERROR: operation not allowed" because of it. A
   * robust client has to be able to recover from its own last failure, not
   * just work from a clean boot.
   *
   * The order matters: hang up the call, drop the context, then re-activate
   * it. Both teardown replies are deliberately ignored; the point is the
   * side effect, not the answer. */
  Serial0.println("\nSESSION: clearing any previous data call");
  Serial0.flush();
  sendAT("ATH", 3000);
  sendAT("AT+CGACT=0,1", 5000);
  delay(1500);
  logLine("reactivate context", "AT+CGACT=1,1");
  logLine("modem-side IP",      "AT+CGPADDR=1");

  /* --- start PPP --- */
  Serial0.println("\nDIAL: ATD*99#");
  Serial0.flush();
  ss.print("ATD*99#\r\n");

  unsigned long t0 = millis();
  String resp = "";
  bool connected = false;
  while (millis() - t0 < DIAL_TIMEOUT) {
    while (ss.available()) resp += (char)ss.read();
    if (resp.indexOf("CONNECT") != -1) { connected = true; break; }
    if (resp.indexOf("NO CARRIER") != -1 || resp.indexOf("ERROR") != -1) break;
    yield();
  }

  if (!connected) {
    Serial0.printf("  dial failed: %s\n", resp.c_str());
    Serial0.println("\nRESULT PPP_NEGOTIATION FAIL - no CONNECT");
    Serial0.flush();
    return;
  }

  Serial0.println("  CONNECT received - handing UART to PPP");
  Serial0.flush();
  resp = "";
  g_dialed = true;

  /* Let the modem finish entering data mode before the first LCP frame.
   *
   * The A7670E is not listening the instant it prints CONNECT. lwIP sends its
   * Configure-Request from ppp_connect() microseconds later, it is swallowed,
   * and the modem then never answers any of the later retransmissions either.
   * The evidence: a hand-built Configure-Request with identical framing, FCS
   * and options gets a reply at T+15s, while the byte-for-byte equivalent frame
   * that lwIP emits at T+0ms is ignored completely. */
  delay(2000);
  {
    uint8_t junk[64];
    size_t dropped = drainUart(junk, sizeof(junk), 100);
    if (dropped > 0) {
      Serial0.printf("  drained %u residual bytes after CONNECT\n", (unsigned)dropped);
      Serial0.flush();
    }
  }

  /* Create the PPPoS session.
   *
   * This uses pppos_create() directly, NOT the pppapi_ wrapper. I originally
   * used pppapi_pppos_create() and it crashed on
   *   assert failed: xQueueSemaphoreTake (( pxQueue ))
   * because this core is built with CONFIG_LWIP_TCPIP_CORE_LOCKING=1. Under
   * that config lwIP does not create a per-thread API semaphore, so the
   * pppapi_ wrapper ends up taking a NULL queue handle. pppos_create() is the
   * call lwIP's own pppos_example.c uses, and it is the correct one here.
   *
   * Received bytes still go in through pppos_input_tcpip(), which is safe from
   * this task and is what the reference example uses for the rx path.
   *
   * Receive config is left at the lwIP defaults (MRU 1500). The setter,
   * ppp_set_recv_config, only exists in the private ppp_impl.h, so there is no
   * supported way to override it from a sketch, and 1500 is what the modem
   * offers anyway. */
  /* Start the lwIP tcpip thread before any PPP traffic.
   *
   * pppos_input_tcpip() posts to the tcpip mbox, and that mbox is only created
   * by tcpip_init(). In this core the WiFi library is what normally calls it,
   * and this sketch never touches WiFi, so the mbox stayed invalid and the
   * first byte the A7670E sent us died on
   *   assert failed: tcpip_inpkt .../api/tcpip.c:257 (Invalid mbox)
   * The assert is in the receive path only, which is why the link appeared to
   * come up and then vanish. */
  tcpip_init(NULL, NULL);

  /* This core is built with CONFIG_LWIP_TCPIP_CORE_LOCKING=1, so every call
   * that touches core state has to run with the core lock held. pppos_create()
   * calls netif_add() internally, which asserts without it:
   *   assert failed: netif_add .../core/netif.c:297
   * (Required to lock TCPIP core functionality!)
   * The same applies to ppp_connect(), which starts the LCP state machine. */
  LOCK_TCPIP_CORE();

  g_pcb = pppos_create(&ppp_netif, pppOut, pppLink, NULL);
  if (g_pcb != NULL) {
    ppp_set_notify_phase_callback(g_pcb, pppPhase);
    ppp_recv_config(g_pcb, PPP_MTU, 0xFFFFFFFFu, 0, 0);
    ppp_send_config(g_pcb, PPP_MTU, 0xFFFFFFFFu, 0, 0);
    ppp_connect(g_pcb, 0);
  }

  UNLOCK_TCPIP_CORE();

  if (g_pcb == NULL) {
    Serial0.println("RESULT PPP_NEGOTIATION FAIL - pppos_create returned NULL");
    Serial0.flush();
    return;
  }

  /* --- negotiate --- */
  uint8_t seen = 0xFF;
  t0 = millis();
  while (millis() - t0 < 15000) {
    if (g_last_phase != seen) {
      seen = g_last_phase;
      Serial0.printf("  PPP phase -> %s\n", phaseName(seen));
      Serial0.flush();
    }
    if (g_got_ip || g_link_err >= 0) break;

    uint8_t buf[256];
    size_t n = drainUart(buf, sizeof(buf), 20);
    if (n > 0) {
      g_rx_bytes += n;
      if (g_rx_bytes <= 128) {
        Serial0.printf("  RX (%u bytes):", (unsigned)n);
        for (size_t i = 0; i < n && i < 48; i++) Serial0.printf(" %02X", buf[i]);
        Serial0.println();
        Serial0.flush();
      }
      pppos_input_tcpip(g_pcb, buf, n);
    }
  }

  /* --- raw LCP probe ---
   * lwIP sent a well-formed LCP Configure-Request and the modem sent nothing
   * back. That has two very different causes: either the modem is not actually
   * in a usable PPP session, or lwIP's session is misconfigured in a way I
   * cannot see from the phase code.
   *
   * To tell them apart, close the lwIP session so nothing else touches the
   * UART, then send a hand-built LCP Configure-Request with a correctly
   * computed FCS-16. If the modem answers a hand-built frame, the hardware and
   * the dial are fine and the problem is on the lwIP side. If it stays silent,
   * the modem never entered a PPP session at all.
   */
  if (g_rx_bytes == 0 && !g_got_ip) {
    Serial0.println("\nPROBE: lwIP got no reply; testing raw LCP framings");
    Serial0.flush();
    /* Deliberately no ppp_close() here. Closing sends a PPP Terminate-Request,
     * the modem answers Terminate-Ack, and the session is then down, so every
     * LCP frame sent afterwards is discarded by design. An earlier version of
     * this probe closed first and the "reply" it saw was that Terminate-Ack
     * arriving late, which made the modem look like it was answering LCP. */
    delay(200);

    rawTrial("MRU=1500, no length field", 1, 1500, false);
    rawTrial("MRU=0,    no length field", 2, 0, false);
    rawTrial("MRU=1500, with length field", 3, 1500, true);
    rawTrial("MRU=0,    with length field", 4, 0, true);
    rawTrial("MRU=1500, no length (again)", 5, 1500, false);
  }

  if (g_got_ip) {
    Serial0.println("\n  LCP + IPCP negotiated. Address assigned by the peer:");
    showIp("PPP-IP");
    Serial0.printf("\nRESULT PPP_NEGOTIATION PASS (phase RUNNING, "
                   "%lu bytes out, %lu bytes in)\n",
                   (unsigned long)g_tx_bytes, (unsigned long)g_rx_bytes);
  } else {
    Serial0.printf("\n  stalled at phase %s", phaseName(g_last_phase));
    if (g_link_err >= 0) Serial0.printf(", link err_code=%d", g_link_err);
    Serial0.printf("\nRESULT PPP_NEGOTIATION FAIL (%lu bytes out, %lu bytes in)\n",
                   (unsigned long)g_tx_bytes, (unsigned long)g_rx_bytes);
  }
  Serial0.flush();
}

void loop() {
  /* After the session is up, keep the link fed. Leaving the UART unattended
   * would let the modem's send buffer overflow and stall the session. */
  if (!g_dialed || !g_got_ip) { delay(1000); return; }

  static uint32_t lastLog = 0;
  uint8_t buf[256];
  size_t n = drainUart(buf, sizeof(buf), 20);
  if (n > 0) {
    g_rx_bytes += n;
    pppos_input_tcpip(g_pcb, buf, n);
  }

  if (millis() - lastLog > 5000) {
    lastLog = millis();
    Serial0.printf("[link] up phase=%s out=%lu in=%lu\n",
                   phaseName(g_last_phase), (unsigned long)g_tx_bytes,
                   (unsigned long)g_rx_bytes);
    Serial0.flush();
  }
}
