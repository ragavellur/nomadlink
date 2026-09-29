/* Known-good A7670E + GNSS bring-up. ESP32-S3 Waveshare V1.
 * Pin map: TX=18, RX=17, PWR rail=21, PWRKEY=42, I2C SDA=3/SCL=2.
 * PWRKEY needs a 1.2s low pulse or the GNSS never cold-starts.
 * NMEA must be routed with CGNSSPORTSWITCH=1,1 (hardware UART), not 0,1.
 * No battery gauge code here: MAX17048 is optional and the read is noisy.
 */

#include <Arduino.h>
#include <Wire.h>
#include <TinyGPS++.h>

#define MODEM_TX_PIN  18
#define MODEM_RX_PIN  17
#define MODEM_PWR_PIN 21
#define MODEM_PWRKEY  42
#define GPSBaud       115200

TinyGPSPlus gps;
HardwareSerial ss(1);
bool networkReady = false;

static void smartDelay(unsigned long ms);
static void printFloat(float val, bool valid, int len, int prec);
static void printInt(unsigned long val, bool valid, int len);
void SentSerial(const char *s);
bool SentMessage(const char *s, unsigned long timeout = 2000);
bool checkNetworkAttached();

void SentSerial(const char *s) {
  while (ss.available()) ss.read();
  for (size_t i = 0; i < strlen(s); i++) { ss.write(s[i]); delay(2); }
  ss.write('\r');
  ss.write('\n');
}

bool SentMessage(const char *s, unsigned long timeout) {
  SentSerial(s);
  unsigned long start = millis();
  String rev = "";
  while (millis() - start < timeout) {
    if (ss.available()) {
      char c = ss.read();
      rev += c;
      if (rev.indexOf("OK") != -1)    { Serial.print("OK: ");    Serial.println(s); return true; }
      if (rev.indexOf("ERROR") != -1) { Serial.print("ERR: ");   Serial.println(s); return false; }
    }
    yield();
  }
  Serial.print("TIMEOUT: "); Serial.println(s);
  return false;
}

/* CGPADDR is the reliable data-layer probe. COPS? lies on this module. */
bool checkNetworkAttached() {
  SentSerial("AT+CGPADDR=1");
  unsigned long start = millis();
  String rev = "";
  while (millis() - start < 3000) {
    if (ss.available()) rev += ss.read();
  }
  int idx = rev.indexOf("+CGPADDR: 1,");
  if (idx != -1) {
    String ip = rev.substring(idx + 12);
    ip.trim();
    if (ip.length() > 4 && ip.indexOf("\"\"") == -1) {
      Serial.println(">>> Data layer up, IP acquired");
      return true;
    }
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("--- A7670E V1 bring-up ---");

  Wire.begin(3, 2);

  /* peripheral power rail */
  pinMode(MODEM_PWR_PIN, OUTPUT);
  digitalWrite(MODEM_PWR_PIN, HIGH);
  delay(500);

  /* PWRKEY: 1.2s low pulse starts the radio core. Skipping this leaves the
     GNSS alive but never cold-started, so it reports a stale almanac with
     every C/N0 field empty. */
  Serial.println("PWRKEY power-on pulse...");
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(100);
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(1200);
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(5000);

  ss.begin(GPSBaud, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  delay(1000);

  int retry = 0;
  while (!SentMessage("AT") && retry < 6) { retry++; delay(1000); }

  SentMessage("AT+CFUN=1");
  delay(500);
  SentMessage("AT+CNMP=38");     /* LTE only: Airtel has retired 2G */
  delay(500);
  SentMessage("AT+CGATT=1");
  delay(500);

  unsigned long netStart = millis();
  while (millis() - netStart < 15000) {
    if (checkNetworkAttached()) { networkReady = true; break; }
    delay(3000);
  }

  /* GNSS */
  Serial.println("Starting GNSS...");
  SentMessage("AT+CGNSSPWR=1");
  delay(1000);
  SentMessage("AT+CGNSSPORTSWITCH=1,1");   /* 1,1 = hardware UART, NOT 0,1 */
  delay(500);
  SentMessage("AT+CGNSSTST=1");
  delay(500);

  Serial.println(F("\nSats HDOP  Latitude   Longitude   Age  Altitude   Speed"));
  Serial.println(F("-------------------------------------------------------"));
}

void loop() {
  if (gps.location.isValid()) {
    Serial.print("LAT="); Serial.println(gps.location.lat(), 6);
    Serial.print("LON="); Serial.println(gps.location.lng(), 6);
  }

  printInt(gps.satellites.value(), gps.satellites.isValid(), 5);
  printFloat(gps.hdop.hdop(), gps.hdop.isValid(), 6, 1);
  printFloat(gps.location.lat(), gps.location.isValid(), 11, 6);
  printFloat(gps.location.lng(), gps.location.isValid(), 12, 6);
  printInt(gps.location.age(), gps.location.isValid(), 5);
  printFloat(gps.altitude.meters(), gps.altitude.isValid(), 7, 2);
  printFloat(gps.speed.kmph(), gps.speed.isValid(), 6, 2);
  Serial.println();

  smartDelay(3000);

  if (millis() > 30000 && gps.charsProcessed() < 10) {
    Serial.println(F("CRITICAL: TinyGPS++ receiving no bytes. Check CGNSSPORTSWITCH=1,1 and PWRKEY."));
  }
}

static void smartDelay(unsigned long ms) {
  unsigned long start = millis();
  do {
    while (ss.available()) gps.encode(ss.read());
    yield();
  } while (millis() - start < ms);
}

static void printFloat(float val, bool valid, int len, int prec) {
  if (!valid) {
    while (len-- > 1) Serial.print('*');
    Serial.print(' ');
  } else {
    Serial.print(val, prec);
    int vi = abs((int)val);
    int flen = prec + (val < 0.0 ? 2 : 1);
    flen += vi >= 1000 ? 4 : vi >= 100 ? 3 : vi >= 10 ? 2 : 1;
    for (int i = flen; i < len; ++i) Serial.print(' ');
  }
}

static void printInt(unsigned long val, bool valid, int len) {
  char sz[32];
  memset(sz, 0, sizeof(sz));
  if (valid) sprintf(sz, "%ld", val);
  else       strcpy(sz, "***************");
  if (len < 32) sz[len] = 0;
  for (int i = strlen(sz); i < len; ++i) sz[i] = ' ';
  if (len > 0) sz[len - 1] = ' ';
  Serial.print(sz);
}
