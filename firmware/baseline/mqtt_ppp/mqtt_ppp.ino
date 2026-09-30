/* TASK-405: MQTT publish over the PPP data channel (BYTE-LEVEL SOCKET PATH).
 *
 * Why this exists: the tracker baseline proved that the module's AT-CIP socket
 * stack is unusable on the physical unit's firmware (A7670M7_V1.11.1). After
 * AT+CGACT=1,1 the UART floods with raw PPP LCP frames, so AT+CIPOPEN can never
 * be answered. The data channel on this modem is PPP and nothing else.
 *
 * So this sketch rides MQTT over PPP: the PPP client core from the ppp_client
 * baseline (proven in TASK-020) brings up lwIP with a real CGNAT address, and
 * then a plain lwIP socket connects to the broker. This removes both parts of
 * BUG-002 forever:
 *   - the AT+CIPSEND '>' prompt no longer exists (no AT in data mode)
 *   - the CONNACK is read with recv() from a socket, not with a line parser,
 *     so "20 02 00 00" is just four bytes of payload
 *
 * The MQTT frame builders (remaining-length encoding, string prep) are the same
 * ones the tracker baseline used; the difference is the transport underneath.
 *
 * Flow: modem up -> register -> ATD*99# -> PPP RUNNING -> resolve broker ->
 * connect -> CONNECT frame -> read CONNACK -> publish -> report rc.
 */

#include <Arduino.h>
#include <lwip/init.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>
#include <lwip/netdb.h>
#include <lwip/dns.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <netif/ppp/pppos.h>
#include <netif/ppp/ppp.h>
#include <netif/ppp/ppp_impl.h>
#include "secrets.h"

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

#ifndef MQTT_HOST
#error "secrets.h must define MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASS, MQTT_TOPIC, CLIENT_ID"
#endif

HardwareSerial ss(1);

static struct netif ppp_netif;
static ppp_pcb *g_pcb = NULL;

static volatile uint8_t  g_last_phase  = 0xFF;
static volatile int       g_link_err    = -1;
static volatile bool      g_got_ip      = false;
static volatile uint32_t g_tx_bytes    = 0;
static volatile uint32_t g_rx_bytes    = 0;
static volatile bool      g_dialed      = false;

/* --- utility -------------------------------------------------------- */

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

static String sendAT(const char *cmd, unsigned long timeout) {
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

static bool escapeDataMode() {
  ss.flush();
  while (ss.available()) ss.read();
  delay(1100);
  ss.print("+++");
  ss.flush();
  delay(1100);
  return modemAnswers(2000, 3);
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
    Serial0.println("PWR-ON: modem already in command mode");
    Serial0.flush();
    return;
  }
  if (escapeDataMode()) return;

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

/* --- MQTT frame builders -------------------------------------------- */

static int putRemainingLength(uint8_t *out, int len) {
  int n = 0;
  do {
    uint8_t d = len % 128;
    len /= 128;
    if (len > 0) d |= 0x80;
    out[n++] = d;
  } while (len > 0);
  return n;
}

static int putString(uint8_t *out, int at, const char *s) {
  int l = strlen(s);
  out[at++] = (l >> 8) & 0xFF;
  out[at++] = l & 0xFF;
  memcpy(out + at, s, l);
  return at + l;
}

/* Build an MQTT CONNECT frame into p. Returns frame length, -1 if too big.
 *
 * The body is laid out starting at fixed-header + 1, then the remaining-length
 * field is written in place; if the encoding needs more than one byte the body
 * is shifted right to make room. A naive "reserve 2 bytes" leaves a garbage
 * byte between the fixed header and the body, which the broker sees as a
 * truncated frame and silently drops - no CONNACK ever comes back. */
static int buildConnect(uint8_t *p, int cap) {
  int n = 1;                           /* p[0] = 0x10 written below */
  int lenPos = n;
  n += 1;                              /* provisional 1-byte length field */
  n = putString(p, n, "MQTT");
  p[n++] = 0x04;                       /* 3.1.1 */
  p[n++] = 0xC2;                       /* user | pass | clean session */
  p[n++] = 0x00; p[n++] = 0x3C;        /* keepalive 60 */
  n = putString(p, n, CLIENT_ID);
  n = putString(p, n, MQTT_USER);
  n = putString(p, n, MQTT_PASS);
  int bodyLen = n - lenPos - 1;
  int rl = putRemainingLength(p + lenPos, bodyLen);
  if (rl > 1) {                        /* rare; make room for the field */
    if (n + rl >= cap) return -1;
    memmove(p + lenPos + rl, p + lenPos + 1, bodyLen);
  }
  p[0] = 0x10;
  return lenPos + rl + bodyLen;
}

static int buildPublish(uint8_t *p, int cap, const char *payload) {
  int n = 1;                           /* p[0] = 0x30 written below */
  int lenPos = n;
  n += 1;                              /* provisional 1-byte length field */
  n = putString(p, n, MQTT_TOPIC);
  int pl = strlen(payload);
  if (n + pl >= cap) return -1;
  memcpy(p + n, payload, pl);
  n += pl;
  int bodyLen = n - lenPos - 1;
  int rl = putRemainingLength(p + lenPos, bodyLen);
  if (rl > 1) {                        /* rare; make room for the field */
    if (n + rl >= cap) return -1;
    memmove(p + lenPos + rl, p + lenPos + 1, bodyLen);
  }
  p[0] = 0x30;                         /* PUBLISH QoS 0 */
  return lenPos + rl + bodyLen;
}

/* Feed the modem UART into PPP until waitMs has elapsed. setup() runs the whole
 * MQTT exchange synchronously, so loop() is NOT draining the UART while the
 * socket ops below block. Leaving the link unattended overflows the modem's
 * send buffer and the PPP session dies (ENETUNREACH), exactly like BUG-006 in
 * nomadlink. Every blocking wait in this sketch must interleave feedPpp(). */
static void feedPpp(unsigned long waitMs) {
  unsigned long t0 = millis();
  while (millis() - t0 < waitMs) {
    uint8_t buf[256];
    size_t n = drainUart(buf, sizeof(buf), 20);
    if (n > 0) {
      g_rx_bytes += n;
      pppos_input_tcpip(g_pcb, buf, n);
    }
  }
}

/* --- socket helpers ----------------------------------------------------

 * IPCP does not reliably push DNS servers on this carrier, and gethostbyname()
 * has nothing to query without them. Fill the resolver with the carrier
 * gateway plus two public resolvers, then it works. */
static void setupDns() {
  static const char *servers[] = { "10.64.64.64", "8.8.8.8", "1.1.1.1" };
  for (int i = 0; i < 3 && i < DNS_MAX_SERVERS; i++) {
    ip_addr_t d;
    if (ipaddr_aton(servers[i], &d)) {
      dns_setserver(i, &d);
      Serial0.printf("  DNS server %d = %s\n", i, servers[i]);
      Serial0.flush();
    }
  }
}

/* The connect wait is a select() on the writable set, NOT a poll of SO_ERROR.
 * A socket still handshaking returns SO_ERROR==0, so polling it declares
 * "CONNECTED" while the SYN is in flight. select() only reports writable once
 * the handshake is really done; SO_ERROR is then read to catch refused/blocked.
 */
static bool waitForConnect(int fd, unsigned timeout_ms) {
  unsigned long t0 = millis();
  while (true) {
    unsigned long remain = timeout_ms - (unsigned long)(millis() - t0);
    if ((long)remain <= 0) remain = 1;

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv;
    tv.tv_sec  = (remain > 3000) ? 3 : (remain / 1000);
    tv.tv_usec = (remain > 3000) ? 0 : ((remain % 1000) * 1000);

    int rc = lwip_select(fd + 1, NULL, &wfds, NULL, &tv);
    if (rc > 0) {
      Serial0.printf("  connect select() -> ready after %lu ms\n",
                     (unsigned long)(millis() - t0));
      Serial0.flush();
      int soerr = 0;
      socklen_t slen = sizeof(soerr);
      if (lwip_getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) != 0 || soerr != 0) {
        if (soerr != 0) Serial0.printf("  connect writable but SO_ERROR=%d\n", soerr);
        Serial0.flush();
        return false;
      }
      return true;
    }
    if (rc < 0) {
      Serial0.printf("  connect select() errno=%d after %lu ms\n",
                     errno, (unsigned long)(millis() - t0));
      Serial0.flush();
      return false;
    }
    /* rc == 0: keep the PPP link alive while the SYN is in flight. */
    feedPpp(200);
    if ((unsigned long)(millis() - t0) >= timeout_ms) {
      Serial0.printf("  connect select() -> timeout after %lu ms\n",
                     (unsigned long)(millis() - t0));
      Serial0.flush();
      return false;
    }
  }
}

/* Wait for a socket to become readable, feeding the PPP link the whole time.
 * The socket must stay non-blocking: a blocking recv() never returns while no
 * data is in flight, so the feed inside the wait loop never runs and the modem
 * UART overflows, killing the session. Returns >0 bytes available, 0 timeout,
 * -1 error, exactly what the caller should hand to recv(). */
static int waitReadable(int fd, unsigned timeout_ms, const char *what) {
  unsigned long t0 = millis();
  while (true) {
    unsigned long remain = timeout_ms - (unsigned long)(millis() - t0);
    if ((long)remain <= 0) remain = 1;

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv;
    tv.tv_sec  = (remain > 3000) ? 3 : (remain / 1000);
    tv.tv_usec = (remain > 3000) ? 0 : ((remain % 1000) * 1000);

    int rc = lwip_select(fd + 1, &rfds, NULL, NULL, &tv);
    if (rc > 0) return 1;
    if (rc < 0) {
      Serial0.printf("  %s select() errno=%d\n", what, errno);
      Serial0.flush();
      return -1;
    }
    feedPpp(200);
    if ((unsigned long)(millis() - t0) >= timeout_ms) {
      Serial0.printf("  %s -> timeout after %lu ms\n", what,
                     (unsigned long)(millis() - t0));
      Serial0.flush();
      return 0;
    }
  }
}

/* Resolve the broker by sending a plain UDP A query and parsing the answer.
 *
 * lwIP's gethostbyname() fails fast on this PPP netif because the ESP-IDF
 * resolver layer is wired to esp_netif, which never sees the raw pppos netif.
 * A raw UDP query sidesteps all of that and exercises the exact socket API the
 * MQTT transport runs over, so it is also a real preflight of the path.
 *
 * Carrier note: this modem/radio has been observed to drop non-TCP traffic
 * (ICMP pings and UDP DNS alike), so DNS over UDP may time out even though the
 * TCP path below it works. secrets.h can define MQTT_HOST_IP to skip the query.
 *
 * Returns 0 and fills addr on success, -1 on failure. */
static int resolveBroker(struct in_addr *addr) {
#ifdef MQTT_HOST_IP
  Serial0.printf("  using MQTT_HOST_IP %s (DNS skipped)\n", MQTT_HOST_IP);
  Serial0.flush();
  if (inet_aton(MQTT_HOST_IP, addr)) return 0;
  Serial0.printf("  MQTT_HOST_IP %s is not a valid IPv4 address\n", MQTT_HOST_IP);
  Serial0.flush();
  return -1;
#else
  static const char *dnsServers[] = { "10.64.64.64", "8.8.8.8", "1.1.1.1" };
  uint16_t tid = 0xB00B;

  int fd = lwip_socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    Serial0.println("  DNS: socket() failed");
    Serial0.flush();
    return -1;
  }

  /* Build an A query. Name is ASCII, max 255, one label per byte. */
  uint8_t q[128];
  memset(q, 0, sizeof(q));
  q[0] = tid >> 8; q[1] = tid & 0xFF;
  q[2] = 0x01; q[3] = 0x00;              /* RD=1 */
  q[4] = 0x00; q[5] = 0x01;              /* QDCOUNT 1 */
  int p = 12;
  const char *label = MQTT_HOST;
  while (*label) {
    const char *dot = strchr(label, '.');
    int l = dot ? (int)(dot - label) : (int)strlen(label);
    q[p++] = (uint8_t)l;
    for (int i = 0; i < l; i++) q[p++] = (uint8_t)label[i];
    label += l;
    if (*label == '.') label++;
  }
  q[p++] = 0;                             /* root */
  q[p++] = 0x00; q[p++] = 0x01;           /* type A */
  q[p++] = 0x00; q[p++] = 0x01;           /* class IN */

  Serial0.printf("  resolving %s via UDP DNS\n", MQTT_HOST);
  Serial0.flush();

  for (int srv = 0; srv < 3; srv++) {
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(53);
    inet_aton(dnsServers[srv], &dst.sin_addr);

    if (lwip_sendto(fd, q, p, 0, (struct sockaddr *)&dst, sizeof(dst)) <= 0) {
      Serial0.printf("  DNS %s: sendto failed (errno=%d)\n", dnsServers[srv], errno);
      Serial0.flush();
      continue;
    }

    lwip_fcntl(fd, F_SETFL, O_NONBLOCK);
    unsigned long t0 = millis();
    while (millis() - t0 < 6000) {
      uint8_t r[512];
      int n = lwip_recvfrom(fd, r, sizeof(r), 0, NULL, NULL);
      if (n > 0) {
        if (n < 12 || r[0] != (tid >> 8) || r[1] != (tid & 0xFF)) {
          lwip_fcntl(fd, F_SETFL, 0);
          break;                          /* stray datagram; next server */
        }
        int qd = (r[5] << 8) | r[6];
        int an = (r[7] << 8) | r[8];
        int off = 12;
        for (int qi = 0; qi < qd && off < n; qi++) {   /* skip question(s) */
          off += 1 + (r[off] & 0x3F);
          while (off < n && r[off] != 0) off += 1 + (r[off] & 0x3F);
          off++;                                        /* root null */
          off += 4;                                     /* QTYPE + QCLASS */
        }
        bool resolved = false;
        for (int a = 0; a < an && off + 11 <= n; a++) {
          if ((r[off] & 0xC0) == 0xC0) off += 2;        /* compressed name */
          else {
            while (off < n && r[off] != 0) off += 1 + (r[off] & 0x3F);
            if (off < n && r[off] == 0) off++;
          }
          if (off + 10 > n) break;
          int atype = (r[off + 0] << 8) | r[off + 1];
          int rdlen = (r[off + 8] << 8) | r[off + 9];
          off += 10;
          if (atype == 1 && rdlen >= 4) {
            memcpy(&addr->s_addr, r + off, 4);
            resolved = true;
          }
          off += rdlen;
          if (resolved) { lwip_close(fd); return 0; }
        }
        lwip_fcntl(fd, F_SETFL, 0);
        break;
      }
      if (n == 0) break;
      if (errno != EAGAIN && errno != EWOULDBLOCK) break;
      feedPpp(50);
    }
  }
  lwip_close(fd);
  Serial0.println("  name resolution failed");
  Serial0.flush();
  return -1;
#endif
}

static void dumpHex(const char *tag, const uint8_t *b, int n) {
  Serial0.printf("  %s[%d]:", tag, n);
  for (int i = 0; i < n; i++) {
    if (i % 16 == 0) Serial0.printf("\n    ");
    Serial0.printf(" %02X", b[i]);
  }
  Serial0.println();
  Serial0.flush();
}

/* --- the actual test -------------------------------------------------- */

static void mqttOverPPP() {
  struct in_addr broker;
  if (resolveBroker(&broker) != 0) {
    Serial0.println("RESULT MQTT_PUBLISH FAIL - name resolution failed");
    Serial0.flush();
    return;
  }
  char btxt[20];
  strlcpy(btxt, inet_ntoa(broker), sizeof(btxt));
  Serial0.printf("  broker %s resolves to %s:%d\n", MQTT_HOST, btxt, MQTT_PORT);
  Serial0.flush();

  int fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    Serial0.println("RESULT MQTT_PUBLISH FAIL - socket() failed");
    Serial0.flush();
    return;
  }

  struct sockaddr_in dst;
  memset(&dst, 0, sizeof(dst));
  dst.sin_family = AF_INET;
  dst.sin_port   = htons(MQTT_PORT);
  dst.sin_addr.s_addr = broker.s_addr;

  lwip_fcntl(fd, F_SETFL, O_NONBLOCK);
  int rc = lwip_connect(fd, (struct sockaddr *)&dst, sizeof(dst));
  if (rc != 0 && errno != EINPROGRESS) {
    Serial0.printf("  connect failed immediately (errno=%d)\n", errno);
    Serial0.flush();
    lwip_close(fd);
    Serial0.println("RESULT MQTT_PUBLISH FAIL - connect() rejected");
    Serial0.flush();
    return;
  }
  if (!waitForConnect(fd, 30000)) {
    Serial0.println("RESULT MQTT_PUBLISH FAIL - TCP connect timed out");
    Serial0.flush();
    lwip_close(fd);
    return;
  }
  Serial0.println("  TCP to broker CONNECTED");
  Serial0.flush();
  /* Socket stays non-blocking for the whole exchange so every wait loop can
   * feed the PPP link; restoring blocking mode made recv()/send() hang while
   * the modem UART sat undrained (ENETUNREACH, BUG-006 class). */
  uint8_t p[512];
  int conLen = buildConnect(p, sizeof(p));
  if (conLen < 0) {
    Serial0.println("RESULT MQTT_PUBLISH FAIL - CONNECT frame too big");
    Serial0.flush();
    lwip_close(fd);
    return;
  }
  dumpHex("CONNECT", p, conLen);

  size_t sent = 0;
  unsigned long wt0 = millis();
  while (sent < (size_t)conLen && millis() - wt0 < 10000) {
    int n = lwip_send(fd, p + sent, conLen - sent, 0);
    if (n > 0) { sent += n; continue; }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { feedPpp(50); continue; }
    Serial0.printf("  CONNECT send failed (errno=%d)\n", errno);
    Serial0.flush();
    lwip_close(fd);
    return;
  }
  if (sent != (size_t)conLen) {
    Serial0.println("  CONNECT send timed out");
    Serial0.flush();
    lwip_close(fd);
    return;
  }
  Serial0.println("  CONNECT frame sent");
  Serial0.flush();

  /* Read the CONNACK: exactly 4 bytes (20 02 <sp> <rc>). This is the whole
   * point of the socket transport - no line parser, CONNACK is just recv().
   * waitReadable() keeps the feed alive so the link survives the wait. The
   * slices are short and the loop keeps going until the 10s window closes, so
   * a slow reply over cellular latency is still accepted. */
  uint8_t connack[4];
  size_t got = 0;
  unsigned long t0 = millis();
  while (got < sizeof(connack) && millis() - t0 < 10000) {
    int wr = waitReadable(fd, 500, "CONNACK");
    if (wr < 0) break;
    if (wr == 0) continue;             /* slice timed out, link fed; keep waiting */
    int n = lwip_recv(fd, connack + got, sizeof(connack) - got, 0);
    if (n > 0) { got += n; continue; }
    if (n == 0) break;                  /* broker closed */
    if (errno != EAGAIN && errno != EWOULDBLOCK) break;
  }
  if (got != 4 || connack[0] != 0x20 || connack[1] != 0x02) {
    Serial0.printf("  CONNACK read got=%u bytes, first=%02X second=%02X (want 20 02)\n",
                   (unsigned)got, connack[0], connack[1]);
    Serial0.flush();
    lwip_close(fd);
    Serial0.println("RESULT MQTT_PUBLISH FAIL - bad CONNACK");
    Serial0.flush();
    return;
  }
  int connackRc = connack[3];
  Serial0.printf("  CONNACK rc=%d sp=%02X\n", connackRc, connack[2]);
  Serial0.flush();
  if (connackRc != 0) {
    lwip_close(fd);
    Serial0.printf("RESULT MQTT_PUBLISH FAIL - broker refused (rc=%d)\n", connackRc);
    Serial0.flush();
    return;
  }
  Serial0.println("MARK-MQTT-CONNECTED");
  Serial0.flush();

  /* Publish. */
  char payload[192];
  snprintf(payload, sizeof(payload),
           "{\"src\":\"mqtt_ppp\",\"uptime_s\":%lu,\"phase\":\"%s\"}",
           (unsigned long)(millis() / 1000), phaseName(g_last_phase));
  int pubLen = buildPublish(p, sizeof(p), payload);
  if (pubLen < 0) {
    Serial0.println("RESULT MQTT_PUBLISH FAIL - PUBLISH frame too big");
    Serial0.flush();
    lwip_close(fd);
    return;
  }

  sent = 0;
  wt0 = millis();
  while (sent < (size_t)pubLen && millis() - wt0 < 10000) {
    int n = lwip_send(fd, p + sent, pubLen - sent, 0);
    if (n > 0) { sent += n; continue; }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { feedPpp(50); continue; }
    Serial0.printf("  PUBLISH send failed (errno=%d)\n", errno);
    Serial0.flush();
    lwip_close(fd);
    return;
  }
  if (sent != (size_t)pubLen) {
    Serial0.println("  PUBLISH send timed out");
    Serial0.flush();
    lwip_close(fd);
    return;
  }
  Serial0.printf("  PUBLISH sent (%d bytes) topic=%s\n", pubLen, MQTT_TOPIC);
  Serial0.flush();

  /* Hang around a moment for the broker to ACK/accept, then close. */
  feedPpp(500);
  lwip_close(fd);
  Serial0.println("RESULT MQTT_PUBLISH PASS");
  Serial0.flush();
}

/* --- setup / PPP bring-up --------------------------------------------- */

void setup() {
  Serial0.begin(115200);
  Serial0.println("\n========== TASK-405: MQTT OVER PPP ==========");
  Serial0.flush();
  delay(1500);

  powerOnModem();
  if (!modemAnswers(2000, 3)) {
    Serial0.println("\nRESULT MQTT_PUBLISH FAIL - modem unreachable");
    Serial0.flush();
    return;
  }
  Serial0.println("MODEM-AT-OK\n");
  Serial0.flush();

  logLine("echo off",           "ATE0");
  logLine("signal",             "AT+CSQ");
  logLine("EPS registration",   "AT+CEREG?");
  logLine("attach",             "AT+CGATT=1");
  logLine("set APN",            "AT+CGDCONT=1,\"IP\",\"" APN "\"");

  Serial0.println("\nSESSION: clearing any previous data call");
  Serial0.flush();
  sendAT("ATH", 3000);
  sendAT("AT+CGACT=0,1", 5000);
  delay(1500);
  logLine("reactivate context", "AT+CGACT=1,1");
  logLine("modem-side IP",      "AT+CGPADDR=1");

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
    Serial0.println("\nRESULT MQTT_PUBLISH FAIL - no CONNECT");
    Serial0.flush();
    return;
  }

  Serial0.println("  CONNECT received - handing UART to PPP");
  Serial0.flush();
  resp = "";
  g_dialed = true;

  /* The A7670E is not listening the instant it prints CONNECT. lwIP sends its
   * Configure-Request microseconds later, it is swallowed, and the modem then
   * never answers retransmissions either. Wait for true data mode first. */
  delay(SETTLE_MS);
  {
    uint8_t junk[64];
    size_t dropped = drainUart(junk, sizeof(junk), 100);
    if (dropped > 0) {
      Serial0.printf("  drained %u residual bytes after CONNECT\n", (unsigned)dropped);
      Serial0.flush();
    }
  }

  /* One lwIP init per process. This sketch never touches WiFi (ppp_client rule:
   * pppos_input_tcpip() posts to the mbox created by tcpip_init()). */
  tcpip_init(NULL, NULL);

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
    Serial0.println("RESULT MQTT_PUBLISH FAIL - pppos_create returned NULL");
    Serial0.flush();
    return;
  }

  uint8_t seen = 0xFF;
  t0 = millis();
  while (millis() - t0 < NEGOTIATE_MS) {
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
      pppos_input_tcpip(g_pcb, buf, n);
    }
  }

  if (!g_got_ip) {
    Serial0.printf("  stalled at phase %s", phaseName(g_last_phase));
    if (g_link_err >= 0) Serial0.printf(", link err_code=%d", g_link_err);
    Serial0.printf("\nRESULT MQTT_PUBLISH FAIL - no PPP IP (%lu out, %lu in)\n",
                   (unsigned long)g_tx_bytes, (unsigned long)g_rx_bytes);
    Serial0.flush();
    return;
  }
  showIp("PPP-IP");
  Serial0.printf("  PPP UP (out=%lu in=%lu)\n",
                 (unsigned long)g_tx_bytes, (unsigned long)g_rx_bytes);
  Serial0.flush();

  /* Make sure the PPP netif is the default route for the socket API. */
  LOCK_TCPIP_CORE();
  if (netif_default == NULL || !ip4_addr_cmp(netif_ip4_addr(&ppp_netif),
                                             netif_ip4_addr(netif_default))) {
    netif_set_default(&ppp_netif);
    Serial0.println("  default netif = PPP netif");
    Serial0.flush();
  }
  UNLOCK_TCPIP_CORE();

  setupDns();

  mqttOverPPP();
}

void loop() {
  /* Feed the PPP link for as long as we are up. This is what keeps the modem's
   * send buffer from overflowing while the socket exchange runs. */
  if (!g_dialed || !g_got_ip) { delay(1000); return; }

  uint8_t buf[256];
  size_t n = drainUart(buf, sizeof(buf), 20);
  if (n > 0) {
    g_rx_bytes += n;
    pppos_input_tcpip(g_pcb, buf, n);
  }
  delay(5);
}