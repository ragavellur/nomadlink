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
#include <netif/ppp/pppapi.h>
#include <netif/ppp/pppos.h>
#include <netif/ppp/ppp.h>

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
  ss.print(cmd); ss.print("\r\n");
  unsigned long t0 = millis();
  String got = "";
  while (millis() - t0 < timeout) {
    while (ss.available()) got += (char)ss.read();
    if (got.indexOf("OK") != -1 || got.indexOf("ERROR") != -1) break;
    yield();
  }
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

  Serial0.println("PWR-ON: probing before any PWRKEY pulse");
  Serial0.flush();
  if (modemAnswers(1500, 3)) {
    Serial0.println("PWR-ON: modem already up, no pulse sent");
    Serial0.flush();
    return;
  }

  Serial0.println("PWR-ON: silent, sending PWRKEY pulse");
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
  logLine("activate context",   "AT+CGACT=1,1");
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

  /* Create the PPPoS session. pppapi_* are the thread-safe wrappers: they hand
   * work to the tcpip thread, which is required because our output callback
   * runs in that thread. */
  g_pcb = pppapi_pppos_create(&ppp_netif, pppOut, pppLink, NULL);
  if (g_pcb == NULL) {
    Serial0.println("RESULT PPP_NEGOTIATION FAIL - pppos_create returned NULL");
    Serial0.flush();
    return;
  }
  pppapi_set_notify_phase_callback(g_pcb, pppPhase);
  pppapi_set_default(g_pcb);
  /* Receive config is left at the lwIP defaults (MRU 1500). The setter,
   * ppp_set_recv_config, only exists in the private ppp_impl.h, so there is no
   * supported way to override it from a sketch. 1500 is what the modem offers
   * anyway, so the default is already correct here. */
  ppp_connect(g_pcb, 0);

  /* --- negotiate --- */
  uint8_t seen = 0xFF;
  t0 = millis();
  while (millis() - t0 < NEGOTIATE_MS) {
    if (g_last_phase != seen) {
      seen = g_last_phase;
      Serial0.printf("  PPP phase -> %s\n", phaseName(seen));
      Serial0.flush();
    }
    if (g_got_ip || g_link_err >= 0) break;

    /* Everything the modem sends from here is PPP, not text. */
    uint8_t buf[256];
    size_t n = drainUart(buf, sizeof(buf), 20);
    if (n > 0) {
      g_rx_bytes += n;
      pppos_input_tcpip(g_pcb, buf, n);
    }
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
