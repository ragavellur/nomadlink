/* NomadLink One — product firmware.
 *
 * This is the traveling router: SoftAP on one side, an IP interface on the
 * other, lwIP forwarding and NAPT in between.
 *
 * Build order is deliberate. Each stage is independently verifiable on the
 * bench, so a failure localises to one stage instead of to "the router":
 *
 *   STAGE 1  SoftAP up, internal DHCP, clients associate and take a lease.
 *   STAGE 2  Modem up, PPP negotiated, PPP netif has a real address.
 *   STAGE 3  Default route out the PPP netif + NAPT on the SoftAP netif.
 *            Clients reach the internet over LTE.
 *
 * STAGE 3 is TASK-109 and is the whole point of this firmware. STA as a second
 * WAN and the failover state machine are layered on afterwards (TASK-102/105/108);
 * dual-WAN integration is TASK-104.
 *
 * WHY ARDUINO AND NOT ESP-IDF
 * ADR-005 records the decision. The short version: NAPT is already compiled into
 * and exported from this core, so NAT is not an ESP-IDF-only capability and the
 * last argument for migrating the product does not hold. PPP was also proven
 * here by TASK-020, after three fixes that are load-bearing below.
 *
 * THE THREE THINGS THAT MUST NOT BE BROKEN
 * These are the BUG-006 fixes. Each one was a real failure and none of them
 * announce themselves clearly:
 *
 *   1. SETTLE_AFTER_CONNECT_MS. The A7670E does not listen the instant it
 *      prints CONNECT. lwIP's first Configure-Request is emitted microseconds
 *      later, is swallowed, and the modem then ignores every retransmission.
 *   2. tcpip_init() — but ONLY if nothing else has. The receive path posts to
 *      the tcpip mbox, which is created by tcpip_init(). The ppp_client baseline
 *      never starts WiFi, so it must call it explicitly. Here SoftAP has already
 *      started the stack, and a second call aborts in
 * *      esp_vfs_lwip_sockets_register. Exactly one thing per process may
 *      initialise lwIP. See BUG-006 and BUG-007.
 *   3. LOCK_TCPIP_CORE around pppos_create()/ppp_connect(). This core is built
 *      with CONFIG_LWIP_TCPIP_CORE_LOCKING=1, so any call touching core state
 *      without the lock asserts.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <lwip/init.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>
#include <netif/ppp/pppos.h>
#include <netif/ppp/ppp.h>
/* ppp_impl.h is nominally private but ships with the core, and is the only way
 * to reach ppp_recv_config()/ppp_send_config(). */
#include <netif/ppp/ppp_impl.h>
/* NAPT. Verified present in the shipped core: liblwip.a contains ip4_napt.c.obj
 * and exports ip_napt_enable_netif(); libesp_netif.a exports
 * esp_netif_napt_enable(). See ADR-005. */
#include <lwip/ip4_napt.h>
#include <esp_netif.h>
#include <esp_mac.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <fcntl.h>
#include <errno.h>

/* --- modem wiring, matching every proven baseline --------------------- */
#define MODEM_TX_PIN  18
#define MODEM_RX_PIN  17
#define MODEM_PWR_PIN 21
#define MODEM_PWRKEY  42
#define MODEM_BAUD    115200

/* --- PPP / LCE --------------------------------------------------------- */
#define PPP_MTU            1500
#define APN                "airtelgprs.com"
#define SETTLE_AFTER_CONNECT_MS 2000
#define DIAL_TIMEOUT_MS    25000
#define NEGOTIATE_TIMEOUT_MS 20000

/* --- SoftAP ----------------------------------------------------------- */
#define AP_SSID_PREFIX     "NomadLink-"
#define AP_GW_IP           "192.168.4.1"
#define AP_DHCP_START      "192.168.4.2"
/* The core's NetworkInterface::config() sets end_ip = start_ip + 10, so the
 * real pool is 11 addresses regardless of the /24 netmask. */
#define AP_DHCP_COUNT      11
#define AP_DHCP_END        "192.168.4.12"
#define STAGE              3

HardwareSerial modem(1);
static struct netif ppp_netif;
static ppp_pcb *g_pcb = NULL;

/* Latched by callbacks that run on the tcpip thread, read from loop(). Printing
 * inside a PPP callback would block the network stack on a slow CDC write. */
static volatile uint8_t  g_last_phase = 0xFF;
static volatile int       g_link_err   = -1;
static volatile bool      g_got_ip     = false;
static volatile uint32_t  g_tx_bytes   = 0;
static volatile uint32_t  g_rx_bytes   = 0;
static volatile bool      g_dialed     = false;
static bool               g_napt_on    = false;
static bool               g_tcpip_ready= false;

/* --- logging ---------------------------------------------------------- */
static void say(const char *fmt, ...) {
  char buf[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial0.println(buf);
  Serial0.flush();
}

/* ------------------------------------------------------------------ *
 *  Modem: command mode
 * ------------------------------------------------------------------ */

static size_t drainUart(uint8_t *buf, size_t len, unsigned long waitMs) {
  unsigned long t0 = millis();
  size_t n = 0;
  while (n < len && millis() - t0 < waitMs) {
    while (modem.available() && n < len) buf[n++] = (uint8_t)modem.read();
    if (n < len) delay(1);
  }
  return n;
}

static String sendAT(const char *cmd, unsigned long timeout) {
  /* Clear anything left over, or a stale "OK" is read as this command's reply
   * and every label ends up paired with the previous command's answer. */
  modem.flush();
  while (modem.available()) modem.read();
  delay(120);

  modem.print(cmd); modem.print("\r\n");
  unsigned long t0 = millis();
  String got = "";
  while (millis() - t0 < timeout) {
    while (modem.available()) got += (char)modem.read();
    if (got.indexOf("\r\nOK") != -1 || got.indexOf("\r\nERROR") != -1 ||
        got.indexOf("+CME ERROR") != -1) break;
    yield();
  }
  delay(60);
  while (modem.available()) got += (char)modem.read();
  return got;
}

static bool modemAnswers(unsigned long perTry, int tries) {
  for (int i = 0; i < tries; i++) {
    if (sendAT("AT", perTry).indexOf("OK") != -1) return true;
    delay(400);
  }
  return false;
}

/* A modem in PPP data mode has no AT parser and silently discards every command,
 * which looks exactly like a dead modem. That misdiagnosis is BUG-005. Escape
 * with the "+++" guard sequence, which needs silence either side. */
static bool escapeDataMode() {
  modem.flush();
  while (modem.available()) modem.read();
  delay(1100);
  modem.print("+++");
  modem.flush();
  delay(1100);
  if (modemAnswers(2000, 3)) {
    say("  '+++' escape accepted - modem was in data mode");
    return true;
  }
  return false;
}

static void logAT(const char *label, const char *cmd) {
  String r = sendAT(cmd, 2000);
  say("  %-22s %s", label, r.length() ? r.c_str() : "(no reply)");
}

static void powerOnModem() {
  pinMode(MODEM_PWR_PIN, OUTPUT);
  digitalWrite(MODEM_PWR_PIN, HIGH);
  delay(800);

  modem.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  delay(400);

  say("PWR-ON: probing before any PWRKEY pulse");
  if (modemAnswers(1500, 3)) {
    say("PWR-ON: modem already in command mode");
    return;
  }
  if (escapeDataMode()) return;

  /* Only a genuinely silent modem gets a PWRKEY pulse. On a live modem a pulse
   * hangs up the call, so it must never be sent speculatively. */
  say("PWR-ON: silent after escape, sending PWRKEY pulse");
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, HIGH); delay(100);
  digitalWrite(MODEM_PWRKEY, LOW);  delay(1200);
  digitalWrite(MODEM_PWRKEY, HIGH); delay(6000);
  say("PWR-ON: %s", modemAnswers(2000, 8) ? "up after PWRKEY" : "STILL SILENT");
}

/* ------------------------------------------------------------------ *
 *  PPP callbacks
 * ------------------------------------------------------------------ */

static u32_t pppOut(ppp_pcb *pcb, const void *data, u32_t len, void *ctx) {
  (void)pcb; (void)ctx;
  const uint8_t *b = (const uint8_t *)data;
  u32_t sent = 0;
  while (sent < len) {
    size_t n = modem.write(b + sent, len - sent);
    if (n == 0) { delayMicroseconds(200); continue; }
    sent += (u32_t)n;
  }
  modem.flush();
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

static const char *phaseName(uint8_t p) {
  switch (p) {
    case PPP_PHASE_DEAD:        return "DEAD";
    case PPP_PHASE_MASTER:      return "MASTER";
    case PPP_PHASE_HOLDOFF:     return "HOLDOFF";
    case PPP_PHASE_INITIALIZE:  return "INITIALIZE";
    case PPP_PHASE_SERIALCONN:  return "SERIALCONN";
    case PPP_PHASE_DORMANT:     return "DORMANT";
    case PPP_PHASE_ESTABLISH:   return "ESTABLISH";
    case PPP_PHASE_AUTHENTICATE:return "AUTHENTICATE";
    case PPP_PHASE_CALLBACK:    return "CALLBACK";
    case PPP_PHASE_NETWORK:     return "NETWORK";
    case PPP_PHASE_RUNNING:     return "RUNNING";
    case PPP_PHASE_TERMINATE:   return "TERMINATE";
    case PPP_PHASE_DISCONNECT:  return "DISCONNECT";
    default:                    return "UNKNOWN";
  }
}

/* ip4addr_ntoa() returns a pointer to a SHARED static buffer, so two calls in
 * one printf make both arguments print the same string - the last one wins.
 * Formatting an address into a caller-owned buffer is the only safe way to
 * print more than one address in a single line. The ppp_client baseline did this
 * with strlcpy; dropping it made local and gateway both read 10.64.64.64 and
 * looked exactly like a real routing fault. */
static const char *addrStr(const ip4_addr_t *a, char *buf, size_t n) {
  strlcpy(buf, ip4addr_ntoa(a), n);
  return buf;
}

static void showIp(const char *when) {
  char abuf[20], gbuf[20];
  say("  %-10s local=%s  gateway=%s  up=%d  mtu=%d", when,
      addrStr(netif_ip4_addr(&ppp_netif), abuf, sizeof(abuf)),
      addrStr(netif_ip4_gw(&ppp_netif),  gbuf, sizeof(gbuf)),
      (int)netif_is_up(&ppp_netif), ppp_netif.mtu);
}

/* Prove the PPP interface can actually carry traffic, independently of NAT.
 * If this fails the WAN is broken and no amount of forwarding will help; if it
 * passes, any remaining failure is in the SoftAP/NAPT path. A bare TCP connect
 * to a public address is used rather than a name lookup so the test does not
 * depend on DNS, which is itself one of the things being tested. */
static bool selftestEgress() {
  int fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) { say("  egress self-test: socket() failed"); return false; }

  struct sockaddr_in dst;
  memset(&dst, 0, sizeof(dst));
  dst.sin_family = AF_INET;
  dst.sin_port   = htons(80);
  inet_pton(AF_INET, "1.1.1.1", &dst.sin_addr);

  lwip_fcntl(fd, F_SETFL, O_NONBLOCK);
  int rc = lwip_connect(fd, (struct sockaddr *)&dst, sizeof(dst));
  if (rc != 0 && errno != EINPROGRESS) {
    say("  egress self-test: connect failed immediately (errno=%d)", errno);
    lwip_close(fd);
    return false;
  }

  /* Poll the non-blocking connect for up to 10 s. */
  bool ok = false;
  unsigned long t0 = millis();
  while (millis() - t0 < 10000) {
    int soerr = 0;
    socklen_t slen = sizeof(soerr);
    if (lwip_getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) == 0 && soerr == 0) {
      ok = true;
      break;
    }
    if (soerr != 0) {
      say("  egress self-test: connect error (errno=%d)", soerr);
      break;
    }
    delay(50);
  }
  lwip_close(fd);
  say("  egress self-test: TCP 1.1.1.1:80 %s", ok ? "CONNECTED" : "TIMED OUT");
  return ok;
}

/* Dump every netif. A router has to be able to explain its own routing table,
 * and this is also how the "NAPT is on the wrong netif" class of bug becomes
 * visible instead of being inferred from a client that silently fails. */
static void dumpNetifs();   /* forward decl; defined after startSoftAP() */

/* ------------------------------------------------------------------ *
 *  STAGE 1 — SoftAP
 * ------------------------------------------------------------------ */

static void startSoftAP() {
  /* Read the MAC from efuse, not from WiFi.macAddress(). The latter returns the
   * WiFi driver's view, which is only valid once the stack is running - reading
   * it first yields a partly-uninitialised buffer, and the SSID then changes on
   * every boot. A client saved the NomadLink-XXXX network would stop matching. */
  uint8_t mac[6] = {0};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    say("  WARNING: esp_read_mac failed, SSID suffix will be unstable");
  }

  char ssid[32];
  snprintf(ssid, sizeof(ssid), "%s%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);

  say("STAGE 1: SoftAP");
  say("  mac          %02X:%02X:%02X:%02X:%02X:%02X",
      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  say("  ssid         %s", ssid);

  WiFi.mode(WIFI_AP);

  /* 192.168.4.1 is both the AP address and the gateway, and is also what gets
   * handed to clients as their DNS server.
   *
   * The third argument is the SUBNET MASK, not another copy of the AP address.
   * Passing 192.168.4.1 there was a silent failure: NetworkInterface::config()
   * calls calculateSubnetCIDR() and bails out with
   *   "Bad netmask. It must be from /24 to /28"
   * BEFORE the DHCP server is ever started. The AP still came up and still
   * broadcast its SSID, so the only symptom was clients that associated and
   * then sat at 169.254.x.x forever. See BUG-008.
   *
   * dns1 (4th arg) is the DHCP lease start for a server interface. The pool is
   * only 11 addresses wide: start_ip to start_ip+10. */
  bool cfg = WiFi.softAPConfig(IPAddress(192, 168, 4, 1),
                               IPAddress(192, 168, 4, 1),
                               IPAddress(255, 255, 255, 0),
                               IPAddress(192, 168, 4, 2));
  say("  ap config     %s", cfg ? "ok" : "FAILED - DHCP will not start");
  if (!cfg) {
    say("  FATAL: softAPConfig rejected the netmask; no leases can be served");
    return;
  }
  /* The PSK is a placeholder for this stage. Real credential handling is
   * TASK-402/TASK-804; hardcoding a shipping password here would be the same
   * mistake TASK-004 cleaned up. */
  bool ok = WiFi.softAP(ssid, "nomadlink-dev");
  delay(500);

  say("  ap up        %s", ok ? "yes" : "NO");
  say("  ap ip        %s", WiFi.softAPIP().toString().c_str());
  say("  clients      %d", WiFi.softAPgetStationNum());
  /* 11 addresses, not the 253 a /24 implies: the core sets
   * end_ip = start_ip + 10 unconditionally. */
  say("  dhcp leases  %s .. %s (%d addresses)", AP_DHCP_START, AP_DHCP_END,
      AP_DHCP_COUNT);
  say("  dns handed to clients: %s (the gateway, so client lookups traverse the NAT)",
      AP_GW_IP);

  /* Starting the AP has a side effect that matters two stages later: the WiFi
   * library has already run esp_netif_init() and tcpip_init() on our behalf, so
   * the tcpip mbox that pppos_input_tcpip() posts to now exists. */
  g_tcpip_ready = true;
}

static void dumpNetifs() {
  LOCK_TCPIP_CORE();
  say("  netif dump:");
  for (struct netif *n = netif_list; n != NULL; n = n->next) {
    char abuf[20], gbuf[20], nbuf[20];
    say("    [%d] %-4s up=%d napt=%d local=%-15s gw=%-15s mtu=%d",
        (int)n->num, n->name, (int)netif_is_up(n), (int)(n->napt != 0),
        addrStr(netif_ip4_addr(n), abuf, sizeof(abuf)),
        addrStr(netif_ip4_gw(n),  gbuf, sizeof(gbuf)), n->mtu);
    say("         netmask=%s", addrStr(netif_ip4_netmask(n), nbuf, sizeof(nbuf)));
  }
  say("  default netif num = %d", (int)(netif_default ? netif_default->num : -1));
  UNLOCK_TCPIP_CORE();
}

/* ------------------------------------------------------------------ *
 *  STAGE 2 — PPP
 * ------------------------------------------------------------------ */

static bool startPPP() {
  say("STAGE 2: modem + PPP");

  powerOnModem();
  if (!modemAnswers(2000, 3)) {
    say("  modem unreachable");
    return false;
  }

  logAT("echo off",         "ATE0");
  logAT("signal",           "AT+CSQ");
  logAT("EPS registration", "AT+CEREG?");
  logAT("attach",           "AT+CGATT=1");
  logAT("set APN",          "AT+CGDCONT=1,\"IP\",\"" APN "\"");

  /* Clear any call left behind by our own last failure. Without this,
   * ATD*99# answers "+CME ERROR: operation not allowed". The order matters:
   * hang up, drop the context, reactivate. Teardown replies are ignored on
   * purpose; the side effect is the point. */
  say("  clearing any previous data call");
  sendAT("ATH", 3000);
  sendAT("AT+CGACT=0,1", 5000);
  delay(1500);
  logAT("reactivate context", "AT+CGACT=1,1");
  logAT("modem-side IP",      "AT+CGPADDR=1");

  say("  dialling ATD*99#");
  modem.print("ATD*99#\r\n");
  unsigned long t0 = millis();
  String resp = "";
  bool connected = false;
  while (millis() - t0 < DIAL_TIMEOUT_MS) {
    while (modem.available()) resp += (char)modem.read();
    if (resp.indexOf("CONNECT") != -1) { connected = true; break; }
    if (resp.indexOf("NO CARRIER") != -1 || resp.indexOf("ERROR") != -1) break;
    yield();
  }
  if (!connected) {
    say("  dial failed: %s", resp.c_str());
    return false;
  }

  g_dialed = true;
  say("  CONNECT received - handing UART to PPP");

  /* BUG-006 fix 1. Non-negotiable: see the header comment. */
  delay(SETTLE_AFTER_CONNECT_MS);
  {
    uint8_t junk[64];
    size_t dropped = drainUart(junk, sizeof(junk), 100);
    if (dropped) say("  drained %u residual bytes after CONNECT", (unsigned)dropped);
  }

  /* The tcpip mbox must exist before the first pppos_input_tcpip(). SoftAP has
   * already done this via esp_netif_init(), so calling tcpip_init() again is
   * not merely redundant here, it is fatal:
   *   ESP_ERROR_CHECK failed: esp_err_t 0x102 (ESP_ERR_INVALID_ARG)
   *   esp_vfs_lwip_sockets_register, vfs_lwip.c:112
   * because the second call re-registers the VFS socket range.
   *
   * BUG-006's fix for the ppp_client baseline is to call tcpip_init()
   * explicitly, and that remains correct there because that sketch never starts
   * WiFi. The rule is contextual, not absolute: exactly one thing in the process
   * may initialise lwIP. See BUG-007. */
  if (!g_tcpip_ready) {
    tcpip_init(NULL, NULL);
    g_tcpip_ready = true;
  } else {
    say("  tcpip already up (started by the WiFi stack) - not calling tcpip_init()");
  }

  /* BUG-006 fix 3. pppos_create() calls netif_add() internally, which asserts
   * without the core lock on this CONFIG_LWIP_TCPIP_CORE_LOCKING=1 build.
   * ppp_connect() starts the LCP state machine and needs it too. */
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
    say("  pppos_create returned NULL");
    return false;
  }

  t0 = millis();
  uint8_t seen = 0xFF;
  while (millis() - t0 < NEGOTIATE_TIMEOUT_MS) {
    if (g_last_phase != seen) {
      seen = g_last_phase;
      say("  PPP phase -> %s", phaseName(seen));
    }
    if (g_got_ip || g_link_err >= 0) break;
    uint8_t buf[256];
    size_t n = drainUart(buf, sizeof(buf), 20);
    if (n > 0) {
      g_rx_bytes += n;
      pppos_input_tcpip(g_pcb, buf, n);
    }
  }

  if (!g_got_ip) {
    say("  stalled at phase %s%s", phaseName(g_last_phase),
        g_link_err >= 0 ? " (link error)" : "");
    return false;
  }
  showIp("PPP-IP");
  say("  PPP UP (out=%lu in=%lu)", (unsigned long)g_tx_bytes, (unsigned long)g_rx_bytes);
  return true;
}

/* ------------------------------------------------------------------ *
 *  STAGE 3 — routing and NAPT
 * ------------------------------------------------------------------ */

static void startRouting() {
  say("STAGE 3: routing + NAPT");

  /* The PPP peer is the gateway for everything that is not the local LAN.
   * netif_set_default() is used rather than ppp_set_default(), because the
   * latter is a macro that expects a ppp_pcb* and dereferences it. */
  LOCK_TCPIP_CORE();
  ppp_netif.flags |= NETIF_FLAG_UP;
  netif_set_default(&ppp_netif);
  UNLOCK_TCPIP_CORE();
  {
    char gbuf[20];
    say("  default route -> PPP peer %s",
        addrStr(netif_ip4_gw(&ppp_netif), gbuf, sizeof(gbuf)));
  }

  /* NAPT goes on the SOFTAP netif, not the PPP netif.
   *
   * ip_napt_forward() begins with `if (!inp->napt) return ERR_OK;` where inp is
   * the netif the packet ARRIVED on. For a SoftAP client reaching the internet,
   * that is the AP netif. Enabling NAPT on the outbound PPP netif instead
   * compiles, boots, and silently drops every client packet.
   *
   * It must also be enabled after the netif is up: ip_napt_enable_netif() is a
   * no-op on a down netif. */
  /* The esp_netif for the AP is registered by the WiFi library under the
   * ifkey "WIFI_AP_DEF" (confirmed present as a string in libesp_netif.a). */
  esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (ap == NULL) {
    say("  no esp_netif for the AP - NAPT not enabled");
    return;
  }
  esp_err_t err = esp_netif_napt_enable(ap);
  g_napt_on = (err == ESP_OK);
  say("  NAPT on SoftAP netif: %s (err=%d)", g_napt_on ? "enabled" : "FAILED", (int)err);
  say("  ~16 KB of the 512-entry NAPT table now allocated from heap");
  dumpNetifs();

  /* Both halves of the data path, separately. If the client half fails, the
   * fault is in forwarding or translation; if the local half fails, the WAN is
   * broken and forwarding is irrelevant. Reporting only one would make the two
   * indistinguishable. */
  say("  local egress (ESP32 itself):");
  selftestEgress();
  say("  client egress (laptop through the NAT) cannot be self-tested;");
  say("    confirm on the client that a page loads.");
}

/* ------------------------------------------------------------------ */

void setup() {
  Serial0.begin(115200);
  say("\n========== NOMADLINK ONE ==========");
  delay(1500);

  startSoftAP();

  bool ppp_ok = false;
#if STAGE >= 2
  ppp_ok = startPPP();
#else
  say("STAGE 2: skipped (STAGE=%d)", STAGE);
#endif

#if STAGE >= 3
  if (ppp_ok) startRouting();
  else say("STAGE 3: skipped - no PPP interface to route through");
#endif

  say("\nREADY. Join '%s' from a phone or laptop.", AP_SSID_PREFIX);
  say("  AP clients : %d", WiFi.softAPgetStationNum());
  say("  PPP        : %s", ppp_ok ? "up" : "down");
  say("  NAPT       : %s", g_napt_on ? "on" : "off");
  say("RESULT %s", (STAGE < 2 || ppp_ok) ? "OK" : "FAIL");
}

void loop() {
  /* Keep the PPP link fed. Leaving the UART unattended lets the modem's send
   * buffer overflow and stalls the session. */
  if (g_dialed && g_pcb && g_got_ip) {
    uint8_t buf[256];
    size_t n = drainUart(buf, sizeof(buf), 20);
    if (n > 0) {
      g_rx_bytes += n;
      pppos_input_tcpip(g_pcb, buf, n);
    }
  }

  /* Report the moment a client associates. "Associated but no lease" and "never
   * associated" are different failures and the status line alone does not tell
   * them apart, which is how a dead DHCP server looks like a WiFi problem. */
  static uint8_t last_clients = 0;
  uint8_t now_clients = WiFi.softAPgetStationNum();
  if (now_clients != last_clients) {
    if (now_clients > last_clients) say("  client associated (%d now)", now_clients);
    else say("  client left (%d now)", now_clients);
    last_clients = now_clients;
  }

  static uint32_t last = 0;
  if (millis() - last > 5000) {
    last = millis();
    say("[status] ap_clients=%d ppp=%s phase=%s napt=%s out=%lu in=%lu",
        now_clients,
        g_got_ip ? "up" : "down",
        phaseName(g_last_phase),
        g_napt_on ? "on" : "off",
        (unsigned long)g_tx_bytes, (unsigned long)g_rx_bytes);
  }
  delay(10);
}
