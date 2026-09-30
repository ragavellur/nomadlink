/* Position service demonstration for TASK-302 (ADR-006 hierarchy).
 *
 * Single position record with an explicit source and accuracy. LBS first
 * (within seconds of boot), then upgrade to GNSS the moment a real fix
 * arrives (quality >= 1 and sats_used > 0). No power cycling mid-session.
 *
 * Proven command sequences on A7670E-FASE rev A7670M7_V1.11.1:
 *   - LBS: AT+CGDCONT/AT+CGATT/AT+CGACT=1,1 then AT+CLBS=1,1 (150s wait),
 *     with AT+SIMEI populated from AT+CGSN. CLBS answers +CLBS: 0,<lon>,<lat>,<acc>.
 *   - GNSS: AT+CGNSSPWR=1 (wait for +CGNSSPWR: READY!), then
 *     AT+CGNSSPORTSWITCH=0,1 (NMEA to UART; 1,1 ERRORs on this rev), then
 *     AT+CGNSSTST=1, parsing NMEA on UART pins 17/18 with TinyGPS++.
 *
 * The two sources cross-checked to well inside the LBS accuracy radius on
 * 2026-09-29/30 (see docs/decisions/ADR-006, TEST-301, TEST-302).
 */

#include <Arduino.h>
#include <TinyGPS++.h>

#define MODEM_RX_PIN  17
#define MODEM_TX_PIN  18
#define MODEM_PWR_PIN 21
#define MODEM_PWRKEY  42
#define MODEM_BAUD    115200

/* 15 minutes of wall-clock time (the value used by the tracker). */
#define FIX_WINDOW_MS 900000UL

#define MAX_ACCURACY_LBS 9999

HardwareSerial ss(1);
TinyGPSPlus gps;
static size_t ln = 0;

/* --- low-level UART helpers --------------------------------------------- */

static bool readLine(char *out, size_t cap, unsigned long deadline) {
  ln = 0;
  while (millis() < deadline) {
    if (!ss.available()) { yield(); continue; }
    char c = (char)ss.read();
    if (c == '$') { while (ss.available()) { if ((char)ss.read() == '\n') break; } ln = 0; continue; }
    if (c == '\n') { if (ln == 0) continue; out[ln] = 0; return true; }
    if (c == '\r') continue;
    if (ln < cap - 1) out[ln++] = c;
  }
  out[ln] = 0;
  return ln > 0;
}

static String sendAT(const char *cmd, unsigned long wait, bool stopOnOk = true) {
  while (ss.available()) ss.read();
  ss.print(cmd); ss.print("\r\n");
  unsigned long deadline = millis() + wait;
  String acc;
  char l[200];
  while (millis() < deadline) {
    if (!readLine(l, sizeof(l), deadline)) break;
    acc += l; acc += '\n';
    if (acc.indexOf("ERROR") >= 0) break;
    if (stopOnOk && l[0] == 'O' && strncmp(l, "OK", 2) == 0) break;
    if (acc.indexOf("+CGNSSPWR:") >= 0 && l[0] == '+') break;
    if (acc.indexOf("+CLBS:") >= 0 && l[0] == '+') break;
  }
  return acc;
}

/* Wake the modem from PPP data mode if a previous session left it there.
 * +++ needs a 1.1s quiet guard before and after per the V.250 escape. */
static bool tryEscToCommandMode() {
  unsigned long before = millis();
  while (millis() - before < 1100) {
    while (ss.available()) ss.read();
    delay(50);
  }
  ss.write((const uint8_t *)"+++", 3); ss.flush();
  delay(1100);
  char line[200];
  bool ok = readLine(line, sizeof(line), millis() + 500);
  if (ok && String(line).indexOf("OK") >= 0) {
    Serial0.println("MODEM-COMMAND-MODE"); Serial0.flush();
    return true;
  }
  return false;
}

/* --- position records ---------------------------------------------------- */

static void emitLbs(double lon, double lat, int accM) {
  Serial0.print("POSITION src=LBS lat="); Serial0.print(lat, 6);
  Serial0.print(" lon="); Serial0.print(lon, 6);
  Serial0.print(" accuracy_m="); Serial0.println(accM);
  Serial0.flush();
}

static void emitGnss() {
  int accM = MAX_ACCURACY_LBS;
  if (gps.hdop.isValid()) accM = (int)(gps.hdop.hdop() * 5.0);
  Serial0.print("POSITION src=GNSS lat=");
  Serial0.print(gps.location.lat(), 6);
  Serial0.print(" lon="); Serial0.print(gps.location.lng(), 6);
  Serial0.print(" alt_m="); Serial0.print(gps.altitude.meters(), 1);
  Serial0.print(" speed_kmh="); Serial0.print(gps.speed.kmph(), 1);
  Serial0.print(" sats="); Serial0.print(gps.satellites.value());
  Serial0.print(" hdop="); Serial0.print(gps.hdop.hdop(), 1);
  Serial0.print(" accuracy_m="); Serial0.println(accM);
  Serial0.flush();
}

/* --- setup -------------------------------------------------------------- */

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

  tryEscToCommandMode();  /* harmless if the modem is already in command mode */

  for (int i = 0; i < 6; i++) {
    if (sendAT("AT", 3000).indexOf("OK") >= 0) break;
    delay(1000);
  }
  Serial0.println("MARK-AT-SYNCED"); Serial0.flush();

  /* Mute any NMEA still streaming from a previous session so the AT+CLBS
   * exchange runs on a clean UART. Stop the pump only; keep the engine
   * powered (no power-cycle, per ADR-006). We re-enable at the GNSS stage. */
  sendAT("AT+CGNSSTST=0", 4000);
  Serial0.println("MARK-GNSS-MUTED"); Serial0.flush();

  /* Start the GNSS engine NOW, concurrently with the LBS query. It acquires
   * while CLBS resolves, per ADR-006. NMEA stays muted (CGNSSTST=0) so the
   * AT+CLBS exchange runs on a clean UART until we start streaming below. */
  sendAT("AT+CGNSSPWR=1", 10000);
  sendAT("AT+CGNSSPORTSWITCH=0,1", 4000);
  Serial0.println("MARK-GNSS-ENGINE-ON"); Serial0.flush();

  /* --- preconditions for LBS (ADR-006) --- */
  sendAT("AT+CNMP=38", 5000);
  sendAT("AT+CGDCONT=1,\"IP\",\"airtelgprs.com\"", 5000);
  sendAT("AT+CGATT=1", 8000);
  delay(5000);
  sendAT("AT+CGACT=1,1", 15000);
  delay(6000);
  String gp = sendAT("AT+CGPADDR=1", 8000);
  Serial0.print("CGPADDR: "); Serial0.println(gp); Serial0.flush();
  String act = sendAT("AT+CGACT?", 8000);
  Serial0.print("CGACT?: "); Serial0.println(act); Serial0.flush();
  String csq = sendAT("AT+CSQ", 5000);
  Serial0.print("CSQ: "); Serial0.println(csq); Serial0.flush();
  String creg = sendAT("AT+CREG?", 5000);
  Serial0.print("CREG?: "); Serial0.println(creg); Serial0.flush();
  Serial0.println("MARK-BEARER-UP"); Serial0.flush();

  String imei = sendAT("AT+CGSN", 5000);
  String digits;
  for (unsigned long i = 0; i < imei.length(); i++) {
    char c = imei[i];
    if (c >= '0' && c <= '9') digits += c;
  }
  Serial0.print("IMEI len="); Serial0.println(digits.length()); Serial0.flush();
  if (digits.length() == 15) {
    String simei = sendAT(("AT+SIMEI=" + digits).c_str(), 8000);
    String simeiQ = sendAT("AT+SIMEI?", 8000);
    Serial0.print("SIMEI="); Serial0.println(simei.indexOf("OK") >= 0 ? "OK" : simei); Serial0.flush();
    Serial0.print("SIMEI?"); Serial0.println(simeiQ); Serial0.flush();
  }

  /* --- LBS first: position within seconds ---
   * AT+CLBS echoes then replies OK BEFORE the +CLBS: URC. Do not break on OK.
   * LBS is a network service and fails transiently (ret_code: 8 busy, 9 open
   * net error, 10 close net error, 11 operation timeout, 12 DNS error). A
   * position service retries instead of publishing nothing on a single miss. */
  sendAT("AT+CSQ", 5000);
  Serial0.println("MARK-LBS-TRY"); Serial0.flush();
  bool lbsOk = false;
  double lbsLon = 0, lbsLat = 0;
  int lbsAcc = 0;
  int lbsCode = -1;
  for (int attempt = 1; attempt <= 3 && !lbsOk; attempt++) {
    String r = sendAT("AT+CLBS=1,1", 30000, false);
    Serial0.print("CLBS RESPONSE "); Serial0.print(attempt); Serial0.print(": ");
    Serial0.println(r); Serial0.flush();
    int pCode = r.indexOf("+CLBS: ");
    if (pCode >= 0) {
      int first = atoi(r.c_str() + pCode + 7);
      lbsCode = first;
      if (first == 0) {
        int q = r.indexOf(",", pCode + 7);
        double lon = 0, lat = 0;
        int acc = 0;
        if (q >= 0) {
          char *end;
          lon = strtod(r.c_str() + q + 1, &end);
          lat = strtod(end + 1, &end);
          acc = atoi(end + 1);
        }
        lbsLon = lon; lbsLat = lat; lbsAcc = acc;
        lbsOk = true;
      } else {
        Serial0.print("LBS ret_code="); Serial0.println(first); Serial0.flush();
      }
    }
    if (!lbsOk) delay(10000);
  }
  if (lbsOk) {
    Serial0.println("MARK-LBS-FIX"); Serial0.flush();
    emitLbs(lbsLon, lbsLat, lbsAcc);
  } else {
    Serial0.print("MARK-LBS-FAIL code="); Serial0.println(lbsCode); Serial0.flush();
  }

  /* --- GNSS concurrently, then upgrade ---
   * Engine is already powered (concurrent acquisition). Enable the NMEA
   * stream now that the LBS exchange is done; the wait loop upgrades the
   * position from LBS to GNSS the moment a fix is usable (ADR-006). */
  sendAT("AT+CGNSSTST=1", 4000);
  Serial0.println("MARK-STREAMING"); Serial0.flush();

  bool haveFix = false;
  unsigned long deadline = millis() + FIX_WINDOW_MS;
  unsigned long nextReport = millis() + 20000;
  while (millis() < deadline && !haveFix) {
    while (ss.available()) gps.encode(ss.read());
    if (gps.location.isValid() && gps.satellites.value() > 0) {
      Serial0.println("LOCK at t+" + String((millis() - deadline + FIX_WINDOW_MS) / 1000) + "s");
      Serial0.println("MARK-GNSS-FIX"); Serial0.flush();
      emitGnss();
      haveFix = true;
    } else if (millis() >= nextReport) {
      nextReport += 20000;
      Serial0.print("searching satsInView=");
      Serial0.print(gps.satellites.value());
      Serial0.print(" hdop=");
      Serial0.print(gps.hdop.isValid() ? gps.hdop.hdop() : 0.0);
      Serial0.println();
      Serial0.flush();
    }
    delay(10);
  }

  Serial0.println(haveFix ? "RESULT=POSITION-SERVICE-OK" : "RESULT=POSITION-SERVICE-NOFIX");
  Serial0.flush();
}

void loop() {
  while (ss.available()) gps.encode(ss.read());
  delay(1000);
}