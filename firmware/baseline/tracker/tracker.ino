/* GPS tracker -> MQTT broker over 4G.
 *
 * Socket path uses AT+CIPOPEN / AT+CIPSEND, NOT AT+CIPSTART.
 * On firmware A011B04A7670M7_F, CIPSTART returns ERROR but CIPOPEN works.
 *
 * KNOWN DEFECTS (see docs/project/bugs.json):
 *   BUG-003  The GNSS wait loop uses an iteration counter, not a millis()
 *            deadline, so it exits after seconds instead of the intended wait.
 *            (TASK-301)
 *   BUG-002  FIXED IN TASK-405: the AT+CIPSEND '>' prompt is a bare byte and the
 *            broker CONNACK is a fixed 4-byte frame (20 02 00 <rc>) with no
 *            final newline. The old line-based reader blocked forever on both.
 *            mqttHandshake() now reads the prompt as a single byte and parses
 *            the CONNACK as bytes, and reports the real CONNACK return code
 *            instead of printing MARK-CONNECTED unconditionally.
 *
 * Credentials live in secrets.h, which is gitignored. See secrets.h.example.
 */

#include <Arduino.h>
#include <TinyGPS++.h>
#include "secrets.h"

#define MODEM_RX_PIN  17
#define MODEM_TX_PIN  18
#define MODEM_PWR_PIN 21
#define MODEM_PWRKEY  42
#define MODEM_BAUD    115200

#ifndef MQTT_HOST
#error "secrets.h must define MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASS, MQTT_TOPIC, CLIENT_ID"
#endif

HardwareSerial ss(1);
TinyGPSPlus gps;

static char rxline[128];
static size_t rxlen = 0;
static bool mqttConnected = false;
static unsigned long noiseLines = 0;
static unsigned long atCommands = 0;

/* NMEA lines start with '$'. Never let them reach the AT response buffer,
 * or they flood it and hide the real reply. */
static bool isNoise(const char *s) {
  return s[0] == '$' || s[0] == '#' ||
         (s[0] == '+' && (strncmp(s, "+CGNS", 5) == 0 || strncmp(s, "+CGP", 4) == 0 ||
                          strncmp(s, "+CIP", 4) == 0 || strncmp(s, "+CDS", 4) == 0 ||
                          strncmp(s, "+CGD", 4) == 0 || strncmp(s, "+CGE", 4) == 0 ||
                          strncmp(s, "+CLB", 4) == 0));
}

/* Read one line. Returns false on timeout.
 * NMEA sentences are consumed and discarded so they never reach `out`. */
static bool readLine(char *out, size_t cap, unsigned long deadline, bool *sawNoise) {
  rxlen = 0;
  while (millis() < deadline) {
    if (!ss.available()) { yield(); continue; }
    char c = (char)ss.read();
    if (c == '$') {
      while (ss.available()) { if ((char)ss.read() == '\n') break; }
      noiseLines++;
      if (sawNoise) *sawNoise = true;
      rxlen = 0;
      continue;
    }
    if (c == '\n') {
      if (rxlen == 0) continue;
      out[rxlen] = 0;
      return true;
    }
    if (c == '\r') continue;
    if (rxlen < cap - 1) out[rxlen++] = c;
  }
  out[rxlen] = 0;
  return rxlen > 0;
}

static String sendAT(const char *cmd, unsigned long wait, const char *expect) {
  while (ss.available()) ss.read();
  atCommands++;
  ss.print(cmd); ss.print("\r\n");
  unsigned long t0 = millis();
  unsigned long deadline = t0 + wait;
  String acc;
  char line[128];
  while (millis() < deadline) {
    if (!readLine(line, sizeof(line), deadline, nullptr)) break;
    acc += line; acc += '\n';
    if (expect && acc.indexOf(expect) >= 0) return acc;
    if (acc.indexOf("ERROR") >= 0) return acc;
  }
  return acc;
}

static void log(const char *tag, const String &r) {
  Serial0.print("### "); Serial0.print(tag); Serial0.println();
  for (unsigned long i = 0; i < r.length(); i++) {
    char c = r[i];
    Serial0.print((c == '\n') ? '/' : c);
  }
  Serial0.println();
  Serial0.flush();
}

static void raw(const uint8_t *b, int n) {
  for (int i = 0; i < n; i++) ss.write(b[i]);
  ss.flush();
}

/* The AT+CIPSEND '>' prompt is a bare byte with no trailing newline, and the
 * broker CONNACK is a raw 4-byte MQTT frame (20 02 <sp> <rc>) that is likewise
 * not newline-terminated. Both are unreadable by readLine(). These helpers
 * operate at byte level and filter NMEA noise so nothing reaches the frame
 * decoder. */
static int connackState = 0;
static uint8_t connackRc = 0;

static void connackReset() { connackState = 0; connackRc = 0xFF; }

/* Feeds one byte into a small state machine for the CONNACK frame.
 * Returns true once the full 4-byte frame (20 02 <sp> <rc>) has been seen,
 * leaving the return code in connackRc. */
static bool connackFeed(uint8_t c) {
  switch (connackState) {
    case 0:
      if (c == 0x20) connackState = 1;
      break;
    case 1:
      if (c == 0x02) connackState = 2;
      else connackState = (c == 0x20) ? 1 : 0;
      break;
    case 2:
      /* session present flag; ignored */
      connackState = 3;
      break;
    case 3:
      connackRc = c;
      connackState = 0;
      return true;
  }
  return false;
}

/* Wait for a single byte within the deadline, discarding NMEA noise. */
static bool waitForByte(uint8_t want, unsigned long deadline) {
  while (millis() < deadline) {
    if (!ss.available()) { yield(); continue; }
    uint8_t c = (uint8_t)ss.read();
    if (c == '$') {
      while (ss.available()) { if ((uint8_t)ss.read() == '\n') break; }
      noiseLines++;
      rxlen = 0;
      continue;
    }
    if (c == want) return true;
  }
  return false;
}

/* Read the broker CONNACK. Returns the CONNACK return code (0 = accepted) or
 * -1 on timeout. Echoed text such as "SEND OK" cannot trip the decoder because
 * it requires the exact 0x20 0x02 byte pair followed by two payload bytes. */
static int readConnack(unsigned long deadline) {
  connackReset();
  while (millis() < deadline) {
    if (!ss.available()) { yield(); continue; }
    uint8_t c = (uint8_t)ss.read();
    if (c == '$') {
      while (ss.available()) { if ((uint8_t)ss.read() == '\n') break; }
      noiseLines++;
      rxlen = 0;
      connackReset();
      continue;
    }
    if (connackFeed(c)) return connackRc;
  }
  return -1;
}

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

static bool mqttHandshake() {
  uint8_t p[256];
  int n = 0;
  p[n++] = 0x10;                       /* CONNECT */
  int lenPos = n++;
  n += 2;                              /* reserve remaining length */
  n = putString(p, n, "MQTT");
  p[n++] = 0x04;                       /* 3.1.1 */
  p[n++] = 0xC2;                       /* user | pass | clean session */
  p[n++] = 0x00; p[n++] = 0x3C;        /* keepalive 60 */
  n = putString(p, n, CLIENT_ID);
  n = putString(p, n, MQTT_USER);
  n = putString(p, n, MQTT_PASS);
  int rl = putRemainingLength(p + lenPos, n - lenPos - 2);
  int total = n - 1 + rl;

  char c[32];
  snprintf(c, sizeof(c), "AT+CIPSEND=%d", total);
  while (ss.available()) ss.read();
  ss.print(c); ss.print("\r\n");
  /* The '>' prompt is a bare byte, not a newline-terminated line. */
  if (!waitForByte('>', millis() + 4000)) { log("CIPSEND connect", String(c) + " no '>' prompt"); return false; }
  raw(p, lenPos + rl);
  raw(p + lenPos + rl, total - lenPos - rl);

  int rc = readConnack(millis() + 8000);
  Serial0.print("CONNACK rc="); Serial0.println(rc); Serial0.flush();
  return rc == 0;
}

static bool publish(const char *payload) {
  /* Mute the NMEA stream for the duration of the write. The GNSS engine stays
   * powered, so the fix and downloaded ephemeris are preserved. */
  sendAT("AT+CGNSSTST=0", 4000, "OK");
  while (ss.available()) ss.read();

  uint8_t p[320];
  int n = 0;
  p[n++] = 0x30;                       /* PUBLISH QoS 0 */
  int lenPos = n++;
  n += 2;
  n = putString(p, n, MQTT_TOPIC);
  int pl = strlen(payload);
  memcpy(p + n, payload, pl);
  n += pl;
  int rl = putRemainingLength(p + lenPos, n - lenPos - 2);
  int total = n - 1 + rl;

  char c[32];
  snprintf(c, sizeof(c), "AT+CIPSEND=%d", total);
  while (ss.available()) ss.read();
  ss.print(c); ss.print("\r\n");
  if (!waitForByte('>', millis() + 4000)) { log("CIPSEND publish", String(c) + " no '>' prompt"); return false; }
  raw(p, lenPos + rl);
  raw(p + lenPos + rl, total - lenPos - rl);

  unsigned long t0 = millis();
  char line[128];
  while (millis() - t0 < 6000) {
    if (readLine(line, sizeof(line), millis() + 50, nullptr)) {
      Serial0.print("<- "); Serial0.println(line); Serial0.flush();
    }
  }
  Serial0.println("PUBLISH sent");
  Serial0.flush();

  sendAT("AT+CGNSSTST=1", 4000, "OK");
  return true;
}

void setup() {
  Serial0.begin(115200);
  Serial0.println("MARK-START"); Serial0.flush();
  delay(1500);

  pinMode(MODEM_PWR_PIN, OUTPUT);
  digitalWrite(MODEM_PWR_PIN, HIGH);
  delay(500);
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, HIGH); delay(100);
  digitalWrite(MODEM_PWRKEY, LOW);  delay(1200);
  digitalWrite(MODEM_PWRKEY, HIGH); delay(5000);
  Serial0.println("MARK-PWRKEY-DONE"); Serial0.flush();

  ss.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  delay(1000);
  for (int i = 0; i < 6; i++) {
    if (sendAT("AT", 3000, "OK").indexOf("OK") >= 0) break;
    delay(1000);
  }
  Serial0.println("MARK-AT-SYNCED"); Serial0.flush();

  sendAT("AT+CNMP=38", 6000, "OK");
  sendAT("AT+CGDCONT=1,\"IP\",\"airtelgprs.com\"", 6000, "OK");
  sendAT("AT+CGATT=1", 8000, "OK");
  delay(5000);
  sendAT("AT+CGACT=1,1", 12000, "OK");
  delay(5000);
  log("CGPADDR", sendAT("AT+CGPADDR=1", 8000, "OK"));
  Serial0.println("MARK-NET-UP"); Serial0.flush();

  /* Socket + MQTT handshake run BEFORE GNSS streaming.
   * NMEA on the same UART corrupts AT response parsing. */
  char c[80];
  snprintf(c, sizeof(c), "AT+CIPOPEN=0,\"TCP\",\"%s\",%d", MQTT_HOST, MQTT_PORT);
  String r = sendAT(c, 60000, "+CIPOPEN");
  log("CIPOPEN", r);
  Serial0.print("CIPOPEN rc codes: ok1=");
  Serial0.print(r.indexOf("+CIPOPEN: 0,1") >= 0);
  Serial0.print(" already10=");
  Serial0.print(r.indexOf("+CIPOPEN: 0,10") >= 0);
  Serial0.println();
  Serial0.flush();
  if (r.indexOf("+CIPOPEN: 0,") < 0) {
    Serial0.println("CIPOPEN FAILED"); Serial0.flush();
    return;
  }
  Serial0.println("MARK-SOCKET-OPEN"); Serial0.flush();

  mqttConnected = mqttHandshake();
  Serial0.print("noise discarded="); Serial0.print(noiseLines);
  Serial0.println();
  Serial0.println(mqttConnected ? "MARK-CONNECTED" : "MARK-CONNECT-FAILED");
  Serial0.flush();
  if (!mqttConnected) return;

  /* now turn GNSS on and wait for a fix */
  sendAT("AT+CGNSSPWR=1", 5000, "OK");
  sendAT("AT+CGNSSPORTSWITCH=1,1", 5000, "OK");
  sendAT("AT+CGNSSTST=1", 5000, "OK");
  Serial0.println("MARK-STREAMING"); Serial0.flush();

  bool haveFix = false;
  for (unsigned long t = 0; t < 900000UL && !haveFix; t += 1000) {
    while (ss.available()) gps.encode(ss.read());
    if (gps.location.isValid()) {
      char payload[192];
      snprintf(payload, sizeof(payload),
               "{\"lat\":%.6f,\"lon\":%.6f,\"sats\":%d,\"hdop\":%.1f,\"fix_age_s\":%lu}",
               gps.location.lat(), gps.location.lng(),
               (int)gps.satellites.value(), gps.hdop.hdop(),
               (unsigned long)(millis() / 1000));
      Serial0.print("FIX payload="); Serial0.println(payload); Serial0.flush();
      publish(payload);
      haveFix = true;
    } else if (t % 20000 == 0) {
      Serial0.print("searching satsInView=");
      Serial0.print(gps.satellites.value());
      Serial0.print(" hdop=");
      Serial0.print(gps.hdop.hdop());
      Serial0.println();
      Serial0.flush();
    }
    delay(10);
  }
  Serial0.println(haveFix ? "TRACKER-DONE-OK" : "TRACKER-DONE-NOFIX");
  Serial0.flush();
}

void loop() {
  while (ss.available()) gps.encode(ss.read());
  mqttConnected = mqttConnected;
  delay(1000);
}
