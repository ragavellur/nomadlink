/* Long uninterrupted GNSS hold for a cold-start fix.
 * IMPORTANT: no power cycling, no other AT traffic. 17 GPS + 10 GLONASS
 * satellites are in view with C/N0 up to 73, so signal is strong; the receiver
 * just needs continuous power to finish downloading almanac + ephemeris.
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

String send(const char *s, unsigned long t) {
  SentSerial(s);
  unsigned long start = millis();
  rev = "";
  while (millis() - start < t) {
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
  int r = 0;
  while (send("AT", 3000).indexOf("OK") < 0 && r < 6) { r++; delay(1000); }
  Serial0.println("MARK-AT-SYNCED"); Serial0.flush();

  send("AT+CGNSSPWR=1", 4000);
  send("AT+CGNSSPORTSWITCH=1,1", 4000);
  send("AT+CGNSSTST=1", 4000);
  Serial0.println("MARK-STREAMING"); Serial0.flush();

  uint32_t t0 = millis(), last = 0;
  uint32_t nmea = 0;
  while (millis() - t0 < HOLD_MS) {
    while (ss.available()) {
      char c = (char)ss.read();
      if (c == '$') nmea++;
      gps.encode(c);
    }
    if (gps.location.isValid()) {
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

  if (gps.location.isValid()) {
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
