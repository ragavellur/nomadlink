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
#include <esp_event.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_wifi.h>
/* Must come AFTER esp_wifi.h: its header guard #errors out if
 * ESP_WIFI_MAX_CONN_NUM is not already defined, and esp_wifi.h is what defines
 * it (via esp_wifi_types_native.h). Reversing these two includes breaks the
 * build with a deliberately unhelpful "WiFi header mismatch!" message. */
#include <esp_wifi_ap_get_sta_list.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include "web_console.h"

/* --- modem wiring, verified against Waveshare's own materials -----------
 *
 * UART pins and baud are taken verbatim from the vendor's example for this
 * exact board (X7670_Network.ino):
 *     static const int RXPin = 17, TXPin = 18;
 *     static const uint32_t GPSBaud = 115200;
 *
 * The power pin was wrong at 21. Waveshare's FAQ, "How to power off the 4G
 * module?", states the A7670E is software-powered through GPIO33 or GPIO22,
 * active HIGH. GPIO21 drives nothing on this board, so the previous "power on"
 * step was a no-op and the module was never actually enabled. */
#define MODEM_TX_PIN  18
#define MODEM_RX_PIN  17
#define MODEM_PWR_PIN 33
/* GPIO42 is NOT the module's PWRKEY. In the schematic the PWRKEY net runs to
 * the CH343P USB-serial chip, not to an ESP32 GPIO, so the ESP32 cannot pulse
 * it. PWRKEY is therefore not driven from here; the module is expected to come
 * up on its own once the power pin is asserted. */
/* Modem UART baud. 115200 is the verified rate for this module and board, and
 * is the only rate the firmware should ever use.
 *
 * An earlier revision "fixed" this to 921600 on the theory that the serial link
 * was saturating and causing the client's packet loss. That theory was wrong.
 * The measured traffic was tens of bytes per second, roughly three orders of
 * magnitude below the 11.2 KB/s ceiling, and a saturated link produces delay
 * and retransmits, not 38% loss. The change gained nothing and cost a modem
 * wedged at a rate it would not answer at, so it has been reverted.
 *
 * Do not change this without evidence that 115200 is itself the problem. */
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
/* Public resolvers advertised to clients. NOT the AP address: this build has no
 * lwIP DNS server, so pointing clients at the gateway is a silent total DNS
 * outage. See the startSoftAP() comment. Must be network-byte-order uint32. */
#define AP_DNS_PRIMARY     0x08080808u  /* 8.8.8.8  */
#define AP_DNS_SECONDARY   0x01010101u  /* 1.1.1.1  */
#define AP_DNS_PRIMARY_STR   "8.8.8.8"
#define AP_DNS_SECONDARY_STR "1.1.1.1"
#define STAGE              3

/* --- console ----------------------------------------------------------- */
#define CONSOLE_MDNS_NAME   "nomadlink"
#define CONSOLE_PORT        80
/* Ceiling on tracked clients. Matches ESP_WIFI_MAX_CONN_NUM (15) for the
 * esp32s3. Note the real ceiling on getting an ADDRESS is lower: this core is
 * built with CONFIG_LWIP_DHCPS_MAX_STATION_NUM=8, so at most 8 clients can be
 * leased at once even though up to 15 can associate. */
#define CONSOLE_MAX_CLIENTS 15
/* How often to re-sample modem telemetry when the PPP feeder is not running. */
#define CONSOLE_MODEM_POLL_MS 30000UL

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
/* Set when the PPP feeder task must stop. Currently never cleared after being
 * set (that would require re-creating the task), but latched so a future WAN
 * redial can request a clean stop instead of racing the task. */
static volatile bool      g_ppp_feed_stop = false;

/* The console must never touch the UART itself (architecture §8: one task owns
 * all AT traffic). This flag is the authority on whether that owner is running,
 * because g_got_ip is not a safe proxy: PPP can be dialled and still be waiting
 * for an IP, which is exactly the window where an interleaved AT command would
 * corrupt a live LCP exchange. */
static volatile bool      g_feeder_running = false;

/* Modem telemetry, sampled on the UART-owning side and cached for the console.
 * csq < 0 means "never sampled", which the UI renders as NOT SAMPLED rather than
 * as a fabricated 0. */
struct ModemTelemetry {
  int  csq;                 /* raw +CSQ first field, 0..31, 99 = unknown */
  bool registered;          /* +CEREG state == 1 */
  char operatorName[40];    /* as reported by +COPS?, verbatim */
  unsigned long sampledAtMs;
};
static ModemTelemetry g_modem = { -1, false, "-", 0UL };

/* Associated-client table, so the console can show who is on the AP and for how
 * long. The core reports MAC+IP pairs but no join time, so first-seen is ours. */
struct ClientEntry {
  char mac[18];
  char ip[16];
  unsigned long firstSeenMs;
  bool used;
};
static ClientEntry g_clients[CONSOLE_MAX_CLIENTS];

/* The SSID actually broadcast, kept for the console. startSoftAP() runs long
 * before any HTTP request can arrive, so its stack-local buffer is gone by then
 * and re-deriving the name risks reporting something other than what clients
 * really joined. */
static char g_ssid[32] = "";


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

/* Moves the module to MODEM_BAUD_TARGET, falling back to the boot rate.
 *
 * Returns the baud actually in use, so the caller reports the real rate instead
 * of the intended one. Leaving this to fail silently is what made the link
 * unusable: at 921600 with a module still at 115200, every AT command returned
 * nothing and PPP never came up, which looked like a dead modem rather than a
 * baud mismatch.
 *
 * The verification step matters as much as the switch. A module that accepts
 * the IPB= command but fails to actually reconfigure would otherwise leave us
 * talking to nothing with no indication why. */

static void powerOnModem() {
  /* GPIO33 is the documented module power enable, active HIGH. It is asserted
   * LOW first so the module gets a real power cycle rather than being left in
   * whatever state the previous run left it in. */
  pinMode(MODEM_PWR_PIN, OUTPUT);
  digitalWrite(MODEM_PWR_PIN, LOW);
  delay(4000);
  digitalWrite(MODEM_PWR_PIN, HIGH);

  modem.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);

  /* The A7670E needs several seconds after power-up before it answers AT. */
  say("PWR-ON: module enabled on GPIO%d, waiting for boot", MODEM_PWR_PIN);
  delay(8000);

  if (modemAnswers(2000, 3)) {
    say("PWR-ON: modem in command mode");
  } else if (!escapeDataMode()) {
    say("PWR-ON: silent after escape, retrying after a longer wait");
    delay(8000);
    if (!modemAnswers(3000, 6)) {
      say("PWR-ON: STILL SILENT");
    }
  }
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
 * depend on DNS, which is itself one of the things being tested.
 *
 * The connect wait is a select() on the writable set, NOT a poll of SO_ERROR.
 * A socket that is still handshaking returns SO_ERROR==0, so the old poll loop
 * declared "CONNECTED" on the first iteration - about 0 ms after connect() -
 * and closed the socket while the SYN was still in flight. That is how an
 * entirely dead link printed "CONNECTED" and then "send stalled errno=119"
 * (EINPROGRESS) in the same boot: the false success was the connect wait, not
 * the link. select() only reports writable once the handshake is really done. */
static bool waitForConnect(int fd, unsigned timeout_ms) {
  fd_set wfds;
  FD_ZERO(&wfds);
  FD_SET(fd, &wfds);
  struct timeval tv;
  tv.tv_sec  = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;

  unsigned long t0 = millis();
  int rc = lwip_select(fd + 1, NULL, &wfds, NULL, &tv);
  say("  connect select() -> rc=%d after %lu ms", rc, (unsigned long)(millis() - t0));
  if (rc <= 0) return false;

  /* Writable can also mean a failed connect; SO_ERROR now holds the real
   * verdict and is safe to read because the handshake has concluded. */
  int soerr = 0;
  socklen_t slen = sizeof(soerr);
  if (lwip_getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) != 0 || soerr != 0) {
    if (soerr != 0) say("  connect select() writable but SO_ERROR=%d", soerr);
    return false;
  }
  return true;
}

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

  bool ok = waitForConnect(fd, 15000);
  lwip_close(fd);
  say("  egress self-test: TCP 1.1.1.1:80 %s", ok ? "CONNECTED" : "TIMED OUT");
  return ok;
}

/* Measures how many bytes per second the LTE link actually carries.
 *
 * A connection test only proves the pipe is open. It says nothing about whether
 * the pipe is wide enough, and a saturated link looks exactly like a flaky one:
 * huge latency variance and 30-40% loss on ICMP. That is what hid the 115200
 * baud bottleneck for an entire bring-up cycle, so capacity is now measured
 * explicitly rather than inferred from "it connected".
 *
 * A single 6 KB request against 1.1.1.1 is enough: the timing is dominated by
 * the radio link, and the response body size is known. */
static void measureThroughput() {
  const int fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) { say("  throughput: socket() failed"); return; }

  struct sockaddr_in dst;
  memset(&dst, 0, sizeof(dst));
  dst.sin_family = AF_INET;
  dst.sin_port   = htons(80);
  inet_pton(AF_INET, "1.1.1.1", &dst.sin_addr);

  /* Reuse the non-blocking connect. The selector in waitForConnect() is the
   * only reliable "is it really connected" check; a plain blocking connect
   * returned ENETUNREACH (errno 113) immediately after an identical non-blocking
   * connect to the same host had just succeeded, which says the blocking form is
   * the unreliable part, not the route. */
  lwip_fcntl(fd, F_SETFL, O_NONBLOCK);
  int rc = lwip_connect(fd, (struct sockaddr *)&dst, sizeof(dst));
  if (rc != 0 && errno != EINPROGRESS) {
    say("  throughput: connect failed immediately (errno=%d)", errno);
    lwip_close(fd);
    return;
  }
  bool up = waitForConnect(fd, 15000);
  if (!up) {
    say("  throughput: no connection after 15 s");
    lwip_close(fd);
    return;
  }

  /* Restore blocking mode before writing. Leaving the socket non-blocking made
   * every lwip_send() return EWOULDBLOCK, so the request was never put on the
   * wire and the measurement read 0 bytes even though the link was fine. */
  lwip_fcntl(fd, F_SETFL, 0);
  unsigned long t0 = millis();
  /* Report a failed send explicitly. Previously the loop just broke on n <= 0,
   * so a send that never reached the wire was indistinguishable from a network
   * that accepted the request and returned nothing. */
  const char *req = "GET / HTTP/1.0\r\nHost: 1.1.1.1\r\nConnection: close\r\n\r\n";
  size_t want = strlen(req);
  size_t sent = 0;
  while (sent < want) {
    int n = lwip_send(fd, req + sent, want - sent, 0);
    if (n <= 0) {
      say("  throughput: send stalled after %u/%u bytes (errno=%d)", (unsigned)sent, (unsigned)want, errno);
      break;
    }
    sent += n;
  }

  uint32_t rx = 0;
  uint8_t buf[512];
  while (millis() - t0 < 15000) {
    int n = lwip_recv(fd, buf, sizeof(buf), 0);
    if (n > 0) { rx += n; continue; }
    if (n == 0) break;            /* peer closed: we have the whole body */
    if (errno != EAGAIN && errno != EWOULDBLOCK) break;
    delay(10);
  }
  unsigned long dt = millis() - t0;
  lwip_close(fd);

  if (dt == 0) dt = 1;
  say("  throughput: %lu bytes in %lu ms = %.1f KB/s (%.0f kbit/s)",
      (unsigned long)rx, dt, (rx / 1024.0) / (dt / 1000.0), (rx * 8.0) / (dt / 1000.0));
}

/* Measures ICMP loss and latency to the LTE gateway, from the ESP32.
 *
 * The reason this exists: 38% client-side ICMP loss with 89 ms to 3.5 s RTT
 * spread was originally blamed on a saturated 115200 baud UART. That is not
 * supportable. Pings of that size are tens of bytes per second, roughly three
 * orders of magnitude below the 11.2 KB/s ceiling, so the serial pipe cannot
 * be the cause. A saturated link also does not produce loss; it produces delay
 * and retransmits. Loss of that shape points at the radio instead.
 *
 * CSQ was 28 (good, -53 dBm) at boot, so signal strength alone does not
 * explain it either. This test separates the two by pinging the first hop
 * inside the carrier network: bad results here mean the radio link, good results
 * mean the loss is further out or client-side. */
static uint16_t ipChecksum(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t sum = 0;
  while (len > 1) { sum += (uint32_t)((p[0] << 8) | p[1]); p += 2; len -= 2; }
  if (len) sum += (uint32_t)(p[0] << 8);
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return (uint16_t)~sum;
}

static void pingTarget(const char *label, const char *addr) {
  /* IPPROTO_ICMP over SOCK_RAW, not SOCK_DGRAM. An earlier version passed
   * protocol 0 to socket(), which silently produced a *UDP* socket, so the
   * "0/10 replied" reading it produced was meaningless: a UDP datagram to port
   * 0 can never draw a reply regardless of radio conditions. */
  const int fd = lwip_socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (fd < 0) { say("  ping %s: socket() failed (errno=%d)", label, errno); return; }

  struct sockaddr_in dst;
  memset(&dst, 0, sizeof(dst));
  dst.sin_family = AF_INET;
  inet_pton(AF_INET, addr, &dst.sin_addr);

  const int tries = 10;
  int got = 0;
  uint32_t rtt_sum = 0, rtt_min = 0xFFFFFFFF, rtt_max = 0;
  uint8_t buf[128];

  struct timeval tv = { 2, 0 };
  lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  for (int i = 0; i < tries; i++) {
    uint8_t pkt[12 + 8] = { 0x00 };
    pkt[0] = 8;                                  /* type: echo request */
    pkt[1] = 0;                                  /* code */
    pkt[2] = 0; pkt[3] = (uint8_t)i;             /* checksum, filled below */
    pkt[4] = 0; pkt[5] = 0;                      /* identifier */
    pkt[6] = (uint8_t)i; pkt[7] = 0;             /* sequence */
    for (int k = 12; k < 20; k++) pkt[k] = (uint8_t)(0xB0 + k);  /* payload */
    uint16_t ck = ipChecksum(pkt, sizeof(pkt));
    pkt[2] = (uint8_t)(ck >> 8); pkt[3] = (uint8_t)(ck & 0xFF);

    unsigned long t0 = millis();
    if (lwip_sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) break;
    if (lwip_recv(fd, buf, sizeof(buf), 0) < 0) { delay(200); continue; }
    if (buf[0] != 0) { delay(200); continue; }   /* 0 = echo reply */

    uint32_t rtt = millis() - t0;
    got++; rtt_sum += rtt;
    if (rtt < rtt_min) rtt_min = rtt;
    if (rtt > rtt_max) rtt_max = rtt;
    delay(200);
  }
  lwip_close(fd);

  if (got == 0) {
    say("  ping %-14s %-15s 0/%d replied", label, addr, tries);
  } else {
    say("  ping %-14s %-15s %d/%d (%.0f%% loss) avg %lu ms, min %lu, max %lu",
        label, addr, got, tries, (tries - got) * 100.0 / tries,
        (unsigned long)(rtt_sum / got), (unsigned long)rtt_min, (unsigned long)rtt_max);
  }
}

static void measurePing() {
  pingTarget("carrier gw", "10.64.64.64");
  pingTarget("1.1.1.1", "1.1.1.1");
  pingTarget("8.8.8.8", "8.8.8.8");
}

/* Logs the moment the DHCP server hands a client an address.
 *
 * The status line can only report the *count* of associated stations, so
 * "associated but never got a lease" and "got a lease and is now failing" look
 * identical from the console. This event fires once per lease and prints the
 * address, which separates the two.
 *
 * The event runs on the event loop task, so it only touches a counter and prints;
 * it must not block. */
static void onLeaseAssigned(void *arg, esp_event_base_t base, int32_t id,
                            void *data) {
  (void)arg;
  if (base == IP_EVENT && id == IP_EVENT_AP_STAIPASSIGNED) {
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    /* The IDF reports an esp_ip4_addr_t, which is a distinct type from lwIP's
     * ip4_addr_t even though the layouts match. Copy the value into a real
     * ip4_addr_t rather than punning the pointer through a cast.
     *
     * ip4addr_ntoa_r() rather than ip4addr_ntoa(): the plain form returns one
     * shared static buffer, which is a real hazard as soon as two addresses are
     * printed in a single call. */
    ip4_addr_t a;
    a.addr = e->ip_info.ip.addr;
    char buf[20];
    ip4addr_ntoa_r(&a, buf, sizeof(buf));
    say("  DHCP LEASE -> %s (gateway %s)", buf, AP_GW_IP);
  }
}

static void watchLeases() {
  esp_err_t err = esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED,
                                             onLeaseAssigned, NULL);
  say("  lease watcher %s (err=%d)", err == ESP_OK ? "registered" : "FAILED", (int)err);
}

/* ------------------------------------------------------------------------- *
 *  Packet tap on the PPP output path
 *
 *  Everything else in this file is a guess about what a client is doing. The
 *  PPP TX/RX byte counters that pppapi reports cannot distinguish "the client
 *  sent nothing" from "lwIP dropped the packet before it reached the modem",
 *  and both look identical from the console. lwIP's mib2 counters and the
 *  forward-path debug strings are not compiled into the prebuilt liblwip.a, so
 *  the netif output function is the one place a tap can be installed.
 *
 *  Wrapping it counts packets that lwIP actually handed to PPP, which is the
 *  exact boundary where a forwarding or NAPT failure becomes visible:
 *
 *    tap count 0 while a client is browsing  -> nothing arrived from the AP
 *    tap count rising                         -> forwarding works
 *
 *  pbuf_clone() is used because p may be a PBUF_REF and NAPT rewrites the
 *  header in place; reading through p->payload directly is safe here because
 *  this runs inside the tcpip thread, which owns the packet at that point.
 * ---------------------------------------------------------------------- */
static struct netif *g_ppp_netif = NULL;
static netif_output_fn g_ppp_output_orig = NULL;
static volatile uint32_t g_tap_pkts = 0;
static volatile uint32_t g_tap_bytes = 0;

/* netif->output is netif_output_fn, whose return type is err_t, not err_t_t. */
static err_t pppOutputTap(struct netif *netif, struct pbuf *p,
                          const ip4_addr_t *ipaddr) {
  if (p != NULL && p->payload != NULL && p->tot_len >= IP_HLEN) {
    g_tap_pkts++;
    g_tap_bytes += p->tot_len;
  }
  return g_ppp_output_orig(netif, p, ipaddr);
}

static void installTap() {
  if (g_ppp_netif == NULL || g_ppp_output_orig != NULL) {
    return;
  }
  g_ppp_output_orig = g_ppp_netif->output;
  if (g_ppp_output_orig == NULL) {
    say("  tap FAILED: PPP netif has no output function");
    return;
  }
  g_ppp_netif->output = pppOutputTap;
  say("  tap installed on PPP output");
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
  /* Keep a copy for the console: the stack-local buffer is long gone by the
   * time an HTTP request arrives, and re-deriving it would risk reporting a
   * different SSID than the one actually broadcast. */
  strncpy(g_ssid, ssid, sizeof(g_ssid) - 1);
  g_ssid[sizeof(g_ssid) - 1] = 0;

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
*   dns1 (4th arg) is the DHCP lease start for a server interface. The pool is
 *   only 11 addresses wide: start_ip to start_ip+10.
 *
 *   The 5th arg enables the DNS offering, and it MUST be nonzero. Without it the
 *   DHCP offer carries no DNS option at all: NetworkInterface::config() only sets
 *   the OFFER_DNS bit when dns != 0 (NetworkInterface.cpp, "Offer DNS to DHCP
 *   clients"), and there is no way to set that bit afterwards - esp_netif_set_dns_info()
 *   on a running server stores the address but never flips the offer bit. The old
 *   call passed no 5th arg, so clients took a lease, pinged 8.8.8.8 (raw IP, no
 *   DNS involved), and had zero resolvers: every name lookup timed out. */
  bool cfg = WiFi.softAPConfig(IPAddress(192, 168, 4, 1),
                               IPAddress(192, 168, 4, 1),
                               IPAddress(255, 255, 255, 0),
                               IPAddress(192, 168, 4, 2),
                               IPAddress(AP_DNS_PRIMARY));
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
  /* DNS servers handed to clients.
   *
   * This must NOT be the AP address. lwIP's DNS server is compiled out of this
   * build (no dns_setserver()/dns_getserver() among the liblwip.a exports), so
   * pointing clients at 192.168.4.1 means every lookup they make is sent to a
   * port nothing is listening on: raw-IP traffic works, `ping google.com` and
   * every website fails. That is a silent, total DNS outage, and it is the
   * worst possible failure for a travel router because the WAN looks healthy.
   *
   * Advertising public resolvers instead sends client lookups straight out
   * through the NAT, where they are translated like any other UDP packet.
   * 8.8.8.8 and 1.1.1.1 are used because they are both reachable over the Airtel
   * CGNAT link; see the nslookup evidence in TASK-101. */
  esp_netif_t *apnetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (apnetif != NULL) {
    /* esp_netif_set_dns_info() is the supported route and is what Arduino's own
     * NetworkInterface::config() calls internally.
     *
     * esp_netif_dhcps_option(ESP_NETIF_DOMAIN_NAME_SERVER, ...) is NOT usable
     * here: it returns ESP_ERR_ESP_NETIF_INVALID_PARAMS (0x5001 = 20481) because
     * the DHCP server is already running, started by WiFi.softAP() above. It must
     * be called before the server starts, and restarting the server to satisfy it
     * would drop the leases just handed out.
     *
     * The dns_info_t member is the IDF-style union with u_addr.ip4.addr, NOT the
     * plain .addr that the IP_EVENT payload uses; the two structs differ. */
    esp_netif_dns_info_t dns;
    dns.ip.type = IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = AP_DNS_PRIMARY;
    esp_err_t r1 = esp_netif_set_dns_info(apnetif, ESP_NETIF_DNS_MAIN, &dns);
    dns.ip.u_addr.ip4.addr = AP_DNS_SECONDARY;
    esp_err_t r2 = esp_netif_set_dns_info(apnetif, ESP_NETIF_DNS_BACKUP, &dns);
    say("  dns for clients: %s, %s (err=%d/%d)", AP_DNS_PRIMARY_STR, AP_DNS_SECONDARY_STR,
        (int)r1, (int)r2);
    if (r1 != ESP_OK || r2 != ESP_OK) {
      say("  FATAL: clients will get no usable DNS; name lookups will fail");
    }
  } else {
    say("  dns for clients: FAILED, no esp_netif for the AP");
  }
  watchLeases();

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

  /* Sample for the console HERE and nowhere else before the dial. This is the
   * one window where the modem is on, nothing else owns the UART, and no PPP
   * session exists to be corrupted. After the feeder starts, the UART is never
   * idle again, so a live refresh is not safe — the console shows this
   * snapshot's age instead of implying it is current. */
  sampleModemTelemetry();

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
  g_ppp_netif = &ppp_netif;
  LOCK_TCPIP_CORE();
  installTap();
  UNLOCK_TCPIP_CORE();
  say("  ~16 KB of the 512-entry NAPT table now allocated from heap");
  dumpNetifs();

  /* Both halves of the data path, separately. If the client half fails, the
   * fault is in forwarding or translation; if the local half fails, the WAN is
   * broken and forwarding is irrelevant. Reporting only one would make the two
   * indistinguishable. */
  say("  local egress (ESP32 itself):");
  selftestEgress();
  say("  link capacity (serial ceiling is %.1f KB/s at %lu baud):",
      (MODEM_BAUD / 10.0) / 1024.0, (unsigned long)MODEM_BAUD);
  measureThroughput();
  say("  radio health (is the loss the radio, or further out?):");
  measurePing();
  say("  client egress (laptop through the NAT) cannot be self-tested;");
  say("    confirm on the client that a page loads.");
}

/* ------------------------------------------------------------------ */

/* The PPP receive path must be fed continuously, independently of what
 * setup()/loop() happen to be doing. The self-tests in startRouting() block for
 * up to ~80 s (15 s TCP connect timeout, twice, plus three 10-ping rounds). If
 * the UART is only drained from loop(), nothing reads it during those tests:
 * the modem sends LCP/ECHO replies, SYN-ACKs and ICMP replies that pile up
 * unread, and the link looks dead. That mismatch is exactly how a healthy
 * bearer got reported as 0/10 pings and two TCP timeouts. This task owns the
 * receive feed for its whole lifetime. */
static void pppFeederTask(void *arg) {
  (void)arg;
  uint8_t buf[256];
  while (!g_ppp_feed_stop) {
    if (g_dialed && g_pcb != NULL && g_got_ip) {
      size_t n = drainUart(buf, sizeof(buf), 20);
      if (n > 0) {
        g_rx_bytes += n;
        pppos_input_tcpip(g_pcb, buf, n);
      }
    } else {
      vTaskDelay(10);
    }
  }
  vTaskDelete(NULL);
}

static void startPppFeeder() {
  g_ppp_feed_stop = false;
  if (xTaskCreatePinnedToCore(pppFeederTask, "pppFeeder", 4096, NULL,
                              /* Run it above the loop task so the self-tests'
                               * blocking socket waits can never starve it. */
                              5, NULL, 1) != pdPASS) {
    say("  pppFeeder task FAILED to start");
  } else {
    g_feeder_running = true;
    say("  pppFeeder task up on core 1");
  }
}

/* ------------------------------------------------------------------ *
 *  Console — TASK-801 / TASK-802
 *
 *  Modelled on martin-ger/esp32_nat_router: one self-contained page served
 *  from flash, reachable by mDNS, reporting the router's own state. The NAT
 *  and forwarding half of that project is already implemented above (STAGE 3);
 *  this is the web half.
 *
 *  The UI serves only measured values. Anything not integrated is listed as
 *  NOT INTEGRATED with the TASK that owns it, never as a plausible number.
 * ------------------------------------------------------------------ */

static AsyncWebServer g_web(CONSOLE_PORT);
static bool g_mdns_ok = false;

/* Extract the first "quoted field" from an AT response. Used for the operator
 * name in +COPS?, which arrives as +COPS: 0,2,"40490",7 (PLMN) or
 * +COPS: 0,0,"Airtel",7 (name). */
static bool atQuoted(const char *hay, char *out, size_t n) {
  const char *q1 = strchr(hay, '"');
  if (!q1) return false;
  const char *q2 = strchr(q1 + 1, '"');
  if (!q2) return false;
  size_t len = (size_t)(q2 - q1 - 1);
  if (len >= n) len = n - 1;
  memcpy(out, q1 + 1, len);
  out[len] = 0;
  return true;
}

static int atIntAfter(const char *hay, const char *key) {
  const char *p = strstr(hay, key);
  if (!p) return -1;
  p += strlen(key);
  while (*p == ' ') p++;
  if (*p < '0' || *p > '9') return -1;
  return atoi(p);
}

/* Sample CSQ / registration / operator. ONLY call this when no other task owns
 * the UART — see g_feeder_running. Called once from startPPP() while the modem
 * is on and the feeder has not started, then from loop() only while the feeder
 * is idle. The value is a snapshot: with PPP up the feeder owns the UART
 * continuously, so a live refresh is not safe and the UI shows the sample age
 * rather than pretending the number is current. */
static void sampleModemTelemetry() {
  if (g_feeder_running) return;

  String csq = sendAT("AT+CSQ", 1200);
  int v = atIntAfter(csq.c_str(), "+CSQ:");
  if (v >= 0) g_modem.csq = v;

  String cereg = sendAT("AT+CEREG?", 1200);
  int st = atIntAfter(cereg.c_str(), "+CEREG:");
  /* +CEREG: <n>,<state>[,...] — the state is the SECOND field, so parse past
   * the first comma explicitly rather than reusing atIntAfter. */
  {
    const char *p = strstr(cereg.c_str(), "+CEREG:");
    if (p) {
      p = strchr(p, ',');
      if (p) {
        while (*p == ' ' || *p == ',') p++;
        g_modem.registered = (atoi(p) == 1);
      }
    }
  }

  /* AT+COPS? is documented as unreliable on this module (it can report 0 while
   * registered), so it is used ONLY to display the network name and is never
   * used as a gate. Registration above is the authority. */
  String cops = sendAT("AT+COPS?", 1500);
  if (cops.indexOf("+COPS:") >= 0) {
    char name[40];
    if (atQuoted(cops.c_str(), name, sizeof(name))) {
      strncpy(g_modem.operatorName, name, sizeof(g_modem.operatorName) - 1);
      g_modem.operatorName[sizeof(g_modem.operatorName) - 1] = 0;
    }
  }
  g_modem.sampledAtMs = millis();
  say("  modem telemetry: csq=%d registered=%d operator=%s",
      g_modem.csq, (int)g_modem.registered, g_modem.operatorName);
}

/* Refresh the first-seen table with real MAC+IP pairs.
 *
 * WiFi.softAPgetStationInfo() existed in core 2.x and is GONE in 3.x — only
 * softAPgetStationNum() survives. The supported replacement is
 * esp_wifi_ap_get_sta_list() for the MACs, then
 * esp_wifi_ap_get_sta_list_with_ip() to pair them with addresses (it resolves
 * from the DHCP table, falling back to the ARP cache).
 *
 * The alternative — inferring the address from IP_EVENT_AP_STAIPASSIGNED and
 * guessing which MAC it belonged to — would be a heuristic, and a wrong
 * MAC/IP pairing shown as fact is exactly the class of bug this project exists
 * to avoid. So the real API is used, and a client that has associated but has
 * no address yet is shown with "no address" rather than a borrowed one. */
static void refreshClients() {
  ClientEntry seen[CONSOLE_MAX_CLIENTS];
  memset(seen, 0, sizeof(seen));
  int n = 0;

  wifi_sta_list_t wl = {};
  if (esp_wifi_ap_get_sta_list(&wl) != ESP_OK) {
    /* Leave the previous table intact: an empty list here would render as
     * "no clients" when the truth is "could not read the list". */
    return;
  }
  wifi_sta_mac_ip_list_t ipmac = {};
  bool have_ip = (esp_wifi_ap_get_sta_list_with_ip(&wl, &ipmac) == ESP_OK);

  int total = wl.num;
  if (total > CONSOLE_MAX_CLIENTS) total = CONSOLE_MAX_CLIENTS;

  for (int i = 0; i < total; i++) {
    const uint8_t *mac = wl.sta[i].mac;
    snprintf(seen[n].mac, sizeof(seen[n].mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    seen[n].ip[0] = 0;
    if (have_ip && i < ipmac.num && ipmac.sta[i].ip.addr != 0) {
      ip4_addr_t a;
      a.addr = ipmac.sta[i].ip.addr;
      ip4addr_ntoa_r(&a, seen[n].ip, sizeof(seen[n].ip));
    } else {
      /* Associated, but no lease yet. Say so rather than inventing 0.0.0.0. */
      snprintf(seen[n].ip, sizeof(seen[n].ip), "no address yet");
    }

    /* Carry the original first-seen forward if this MAC is already known. */
    seen[n].firstSeenMs = millis();
    for (int k = 0; k < CONSOLE_MAX_CLIENTS; k++) {
      if (g_clients[k].used && strcmp(g_clients[k].mac, seen[n].mac) == 0) {
        seen[n].firstSeenMs = g_clients[k].firstSeenMs;
        break;
      }
    }
    seen[n].used = true;
    n++;
  }

  memcpy(g_clients, seen, sizeof(g_clients));
}

/* JSON string escaping for the operator name, which comes from the modem and
 * may contain characters that would otherwise break the document. */
static void jsonEscape(const char *in, char *out, size_t n) {
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 2 < n; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
    else if (c < 0x20)         { o += snprintf(out + o, n - o, "\\u%04x", c); }
    else                       { out[o++] = (char)c; }
  }
  out[o] = 0;
}

static void buildStatusJson(char *buf, size_t n) {
  char pppLocal[20] = "-", pppPeer[20] = "-", defGw[20] = "-";
  if (g_got_ip && g_ppp_netif != NULL) {
    /* Read under the core lock: netif addresses are owned by the tcpip thread. */
    LOCK_TCPIP_CORE();
    strncpy(pppLocal, addrStr(netif_ip4_addr(g_ppp_netif), pppLocal, sizeof(pppLocal)),
            sizeof(pppLocal) - 1);
    strncpy(pppPeer,  addrStr(netif_ip4_gw(g_ppp_netif), pppPeer, sizeof(pppPeer)),
            sizeof(pppPeer) - 1);
    if (netif_default != NULL) {
      strncpy(defGw, addrStr(netif_ip4_gw(netif_default), defGw, sizeof(defGw)),
              sizeof(defGw) - 1);
    }
    UNLOCK_TCPIP_CORE();
  }

  char opEsc[96];
  jsonEscape(g_modem.operatorName, opEsc, sizeof(opEsc));

  refreshClients();

  int o = snprintf(buf, n,
      "{\"ssid\":\"%s\",\"ap_ip\":\"%s\",\"mdns_ok\":%s,"
      "\"dhcp_start\":\"%s\",\"dhcp_end\":\"%s\","
      "\"dns_prim\":\"%s\",\"dns_sec\":\"%s\","
      "\"ppp_up\":%s,\"ppp_local\":\"%s\",\"ppp_peer\":\"%s\",\"default_gw\":\"%s\","
      "\"napt_on\":%s,\"tx_bytes\":%lu,\"rx_bytes\":%lu,"
      "\"tap_pkts\":%lu,\"tap_bytes\":%lu,"
      "\"csq\":%d,\"csq_valid\":%s,\"registered\":%s,\"operator\":\"%s\","
      "\"csq_age_s\":%ld,\"clients\":%d,\"uptime\":%lu,"
      "\"heap_free\":%lu,\"heap_min\":%lu,"
      "\"psram_free\":%lu,\"psram_total\":%lu,"
      "\"flash_used\":%lu,\"flash_total\":%lu,\"client_list\":[",
      g_ssid, AP_GW_IP, g_mdns_ok ? "true" : "false",
      AP_DHCP_START, AP_DHCP_END, AP_DNS_PRIMARY_STR, AP_DNS_SECONDARY_STR,
      g_got_ip ? "true" : "false", pppLocal, pppPeer, defGw,
      g_napt_on ? "true" : "false",
      (unsigned long)g_tx_bytes, (unsigned long)g_rx_bytes,
      (unsigned long)g_tap_pkts, (unsigned long)g_tap_bytes,
      g_modem.csq, g_modem.csq >= 0 ? "true" : "false",
      g_modem.registered ? "true" : "false", opEsc,
      (long)((g_modem.sampledAtMs == 0) ? -1
                                         : (millis() - g_modem.sampledAtMs) / 1000),
      (int)WiFi.softAPgetStationNum(), (unsigned long)(millis() / 1000),
      (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
      (unsigned long)ESP.getFreePsram(), (unsigned long)ESP.getPsramSize(),
      (unsigned long)ESP.getSketchSize(), (unsigned long)ESP.getFlashChipSize());

  if (o < 0 || (size_t)o >= n) { buf[0] = 0; return; }

  for (int i = 0; i < CONSOLE_MAX_CLIENTS && g_clients[i].used; i++) {
    int w = snprintf(buf + o, n - o, "%s{\"mac\":\"%s\",\"ip\":\"%s\",\"since_s\":%lu}",
                     (i == 0) ? "" : ",", g_clients[i].mac, g_clients[i].ip,
                     (unsigned long)((millis() - g_clients[i].firstSeenMs) / 1000));
    if (w < 0 || (size_t)(o + w) >= n) break;
    o += w;
  }
  snprintf(buf + o, n - o, "]}");
}

static void startConsole() {
  refreshClients();

  g_web.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send_P(200, "text/html", CONSOLE_HTML);
  });

  g_web.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *r) {
    /* One static buffer, reused. ESPAsyncWebServer copies the payload before
     * the handler returns, so this is safe and avoids a per-request heap
     * allocation on a device where the NAPT table has already taken ~16 KB. */
    static char json[1400];
    buildStatusJson(json, sizeof(json));
    r->send(200, "application/json", json);
  });

  /* Anything else is the console's single page. A phone that requests a
   * missing asset should still land on the console rather than a 404. */
  g_web.onNotFound([](AsyncWebServerRequest *r) {
    r->send_P(200, "text/html", CONSOLE_HTML);
  });

  g_web.begin();
  say("  console http://%s (port %d)", AP_GW_IP, CONSOLE_PORT);

  g_mdns_ok = MDNS.begin(CONSOLE_MDNS_NAME);
  if (g_mdns_ok) {
    MDNS.addService("http", "tcp", CONSOLE_PORT);
    say("  mDNS %s.local up", CONSOLE_MDNS_NAME);
  } else {
    say("  mDNS FAILED - console still reachable at http://%s", AP_GW_IP);
  }
}

void setup() {
  Serial0.begin(115200);
  say("\n========== NOMADLINK ONE ==========");
  delay(1500);

  startSoftAP();
  startConsole();


  bool ppp_ok = false;
#if STAGE >= 2
  ppp_ok = startPPP();
#else
  say("STAGE 2: skipped (STAGE=%d)", STAGE);
#endif

#if STAGE >= 3
  if (ppp_ok) {
    /* Start the UART feed before the self-tests: they block this task for ~80 s
     * and the modem must keep being drained throughout. */
    startPppFeeder();
    startRouting();
  } else say("STAGE 3: skipped - no PPP interface to route through");
#endif

  say("\nREADY. Join '%s' from a phone or laptop.", AP_SSID_PREFIX);
  say("  AP clients : %d", WiFi.softAPgetStationNum());
  say("  PPP        : %s", ppp_ok ? "up" : "down");
  say("  NAPT       : %s", g_napt_on ? "on" : "off");
  say("RESULT %s", (STAGE < 2 || ppp_ok) ? "OK" : "FAIL");
}

void loop() {
  /* The PPP receive path is owned by the pppFeeder task (see startPppFeeder()).
   * Feeding from here too would double-drain a single UART. loop() only watches
   * the client side and reports status. */

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

  /* Keep the console's client table current so "since" and the MAC/IP list stay
   * truthful between page loads. Cheap, and it does not touch the UART. */
  static uint32_t last_refresh = 0;
  if (millis() - last_refresh > 2000) {
    last_refresh = millis();
    refreshClients();
  }

  /* Refresh modem telemetry ONLY while the PPP feeder is not running. With PPP
   * up the feeder owns the UART for the life of the session, and an interleaved
   * AT command would corrupt live LCP traffic — so the snapshot simply ages and
   * the UI shows its age. Never send AT here speculatively. */
  if (!g_feeder_running) {
    static uint32_t last_modem = 0;
    if (millis() - last_modem > CONSOLE_MODEM_POLL_MS) {
      last_modem = millis();
      if (g_modem.sampledAtMs != 0) sampleModemTelemetry();
    }
  }

  static uint32_t last = 0;
  if (millis() - last > 5000) {
    last = millis();
    /* Deltas, not totals. A client that associates and then generates real
     * traffic must make the PPP byte counters climb. If a client is browsing
     * and the deltas stay at zero, its packets are not reaching the LTE netif at
     * all, which localises the fault to forwarding rather than to NAPT,
     * the modem, or the carrier. */
    static uint32_t prev_out = 0, prev_in = 0;
    uint32_t d_out = g_tx_bytes - prev_out;
    uint32_t d_in  = g_rx_bytes - prev_in;
    prev_out = g_tx_bytes;
    prev_in  = g_rx_bytes;
    say("[status] clients=%d ppp=%s napt=%s @%lu | ppp_bytes +%lu out / +%lu in  (totals %lu/%lu) | fwd_tap=%lu pkts/%lu B",
        now_clients,
        g_got_ip ? "up" : "down",
        g_napt_on ? "on" : "off",
        (unsigned long)MODEM_BAUD,
        (unsigned long)d_out, (unsigned long)d_in,
        (unsigned long)g_tx_bytes, (unsigned long)g_rx_bytes,
        (unsigned long)g_tap_pkts, (unsigned long)g_tap_bytes);
  }
  delay(10);
}
