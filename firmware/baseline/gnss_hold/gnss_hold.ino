/* Long uninterrupted GNSS hold for a cold-start fix.
 * IMPORTANT: no power cycling, no other AT traffic. 17 GPS + 10 GLONASS
 * satellites are in view with C/N0 up to 73, so signal is strong; the receiver
 * just needs continuous power to finish downloading almanac + ephemeris.
 *
 * This is the TASK-301 demonstration baseline:
 *   - a real millis() deadline (HOLD_MS), never an iteration counter (BUG-003)
 *   - a fix is only accepted with TinyGPS++ location validity AND satsUsed > 0,
 *     matching the ADR-006 quality gate
 *   - the module is never power-cycled mid-session
 */

#include <Arduino.h>
#include <TinyGPS++.h>

#define MODEM_TX_PIN  18
#define MODEM_RX_PIN  17
#define MODEM_PWR_PIN 21
#define MODEM_PWRKEY  42
#define GPSBaud       115200
#define HOLD_MS       1200000UL   /* 20 minutes */

HardwareSerial ss(1);
TinyGPSPlus gps;
String rev;

void SentSerial(const char *s) {
  while (ss.available()) ss.read();
  for (size_t i = 0; i < strlen(s); i++) { ss.write(s[i]); delay(2); }
  ss.write('\r'); ss.write('\n');
}

String send(const char *label, const char *s, unsigned long t) {
  SentSerial(s);
  unsigned long start = millis();
  rev = "";
  while (millis() - start < t) {
    while (ss.available()) { char c = ss.read(); rev += c; }
    if (rev.indexOf("OK") != -1 || rev.indexOf("ERROR") != -1) break;
    yield();
  }
  String trimmed = rev;
  trimmed.trim();
  if (trimmed.length() > 0) {
    Serial0.print("<- [");
    Serial0.print(label);
    Serial0.print("] ");
    Serial0.println(trimmed);
    Serial0.flush();
  }
  return rev;
}

/* Raw AT probe that only reads, never logs. Returns reply. */
String rawAT(const char *s) {
  unsigned long start = millis();
  rev = "";
  SentSerial(s);
  while (millis() - start < 1500) {
    while (ss.available()) { char c = ss.read(); rev += c; }
    if (rev.indexOf("OK") != -1 || rev.indexOf("ERROR") != -1) break;
    yield();
  }
  return rev;
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

  ss.begin(GPSBaud, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  delay(1000);

  /* The modem may still be in PPP data mode from a previous session; AT is
   * silent until it is escaped back to command mode. Probe first, then try
   * +++, then PWRKEY. */
  bool ok = rawAT("AT").indexOf("OK") >= 0;
  if (!ok) {
    ss.flush();
    while (ss.available()) ss.read();
    delay(1100);
    ss.print("+++");
    ss.flush();
    delay(1100);
    ok = rawAT("AT").indexOf("OK") >= 0;
  }
  Serial0.println(ok ? "MODEM-COMMAND-MODE" : "MODEM-SILENT");
  Serial0.flush();
  int r = 0;
  while (send("ATSYNC", "AT", 3000).indexOf("OK") < 0 && r < 6) { r++; delay(1000); }
  Serial0.println("MARK-AT-SYNCED"); Serial0.flush();

  send("GNSSPWR", "AT+CGNSSPWR=1", 10000);
  /* A76XX app note: after powering the GNSS engine, wait for the
   * +CGNSSPWR: READY! indication before any other GNSS operation. */
  {
    unsigned long r0 = millis();
    while (millis() - r0 < 10000 && rev.indexOf("READY!") < 0) {
      while (ss.available()) rev += (char)ss.read();
      delay(20);
    }
    Serial0.println(rev.indexOf("READY!") >= 0 ? "GNSS-ENGINE-READY" : "GNSS-ENGINE-NO-READY");
    Serial0.flush();
  }
  /* Waveshare board doc: NMEA to the ESP32 UART pins (17/18) is
   * AT+CGNSSPORTSWITCH=0,1. The 1,1 form the original sketch used is
   * rejected with ERROR, so NMEA never reached the UART. */
  send("PORTSW", "AT+CGNSSPORTSWITCH=0,1", 4000);
  send("TST", "AT+CGNSSTST=1", 4000);
  Serial0.println("MARK-STREAMING"); Serial0.flush();

  uint32_t t0 = millis(), last = 0;
  uint32_t nmea = 0;
  while (millis() - t0 < HOLD_MS) {
    while (ss.available()) {
      char c = (char)ss.read();
      if (c == '$') nmea++;
      gps.encode(c);
    }
    /* ADR-006 gate: location validity AND a real satellite lock, not a bogus
     * lat/lon with zero sats. */
    if (gps.location.isValid() && gps.satellites.value() > 0) {
      Serial0.print("LOCK at t+" + String((millis() - t0) / 1000) + "s  sats=");
      Serial0.print(gps.satellites.value());
      Serial0.print("  hdop="); Serial0.print(gps.hdop.hdop(), 1);
      Serial0.println();
      Serial0.flush();
      break;
    }
    if (millis() - last > 20000) {
      last = millis();
      Serial0.print("t+" + String((millis() - t0) / 1000) + "s  satsUsed=");
      Serial0.print(gps.satellites.isValid() ? gps.satellites.value() : 0);
      Serial0.print("  hdop=");
      Serial0.print(gps.hdop.isValid() ? gps.hdop.hdop() : 0.0, 1);
      Serial0.print("  age=");
      Serial0.print(gps.location.age() / 1000);
      Serial0.print("s  sentences=");
      Serial0.print(nmea);
      Serial0.print("  fix=");
      Serial0.println(gps.location.isValid() ? "YES" : "no");
      Serial0.flush();
    }
    delay(5);
  }

  if (gps.location.isValid() && gps.satellites.value() > 0) {
    Serial0.println("RESULT=LOCK");
    Serial0.print("LAT="); Serial0.println(gps.location.lat(), 6);
    Serial0.print("LON="); Serial0.println(gps.location.lng(), 6);
    Serial0.print("ALT="); Serial0.println(gps.altitude.meters(), 1);
  } else {
    Serial0.println("RESULT=NOLOCK");
  }
  Serial0.println("HOLD-DONE");
  Serial0.flush();
}

void loop() { delay(1000); }
