/* MODULE HTTP DIAGNOSTIC - does the A7670E's own stack carry data?
 *
 * Context: PPP over the ESP32 comes up (LCP/IPCP negotiate, IP 100.x.x.x
 * assigned) but carries no data - TCP to 1.1.1.1:80 times out and even the
 * carrier gateway returns 0/10 ICMP. Every AT+CIP* command errors instantly,
 * which on this firmware means the legacy SIMCom socket stack is absent.
 *
 * The vendor's own example for the SIM7670X sibling board uses the HTTP stack
 * instead: AT+HTTPINIT / AT+HTTPPARA / AT+HTTPACTION. If THAT works, the module
 * and the carrier are fine and the fault sits in our ESP32 PPP/lwIP path. If it
 * also fails, the module or the SIM data session is the fault.
 *
 * Everything is printed verbatim so an error code is visible, not a bare ERROR.
 */

#include <Arduino.h>

static const int RXPin = 17, TXPin = 18;
static const uint32_t GPSBaud = 115200;

static HardwareSerial modem(1);

static String readFor(unsigned long ms) {
  String r = "";
  unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    while (modem.available()) r += (char)modem.read();
    delay(10);
  }
  delay(200);
  while (modem.available()) r += (char)modem.read();
  return r;
}

static String oneLine(String s) {
  s.replace("\r\n", " | ");
  s.replace('\r', ' ');
  s.replace('\n', ' ');
  s.trim();
  return s;
}

static String send(const char *c, unsigned long wait_ms) {
  while (modem.available()) modem.read();
  modem.print(c);
  modem.print("\r\n");
  return readFor(wait_ms);
}

static void step(const char *label, const char *cmd, unsigned long wait_ms) {
  String r = oneLine(send(cmd, wait_ms));
  Serial0.printf("%-40s -> %s\n", label, r.length() ? r.c_str() : "(empty)");
}

void setup() {
  Serial0.begin(115200);
  delay(1500);
  Serial0.println("=== MODULE HTTP DIAGNOSTIC ===");

  pinMode(TXPin, OUTPUT);
  digitalWrite(TXPin, HIGH);
  pinMode(RXPin, INPUT);
  modem.end();
  delay(200);
  modem.begin(GPSBaud, SERIAL_8N1, RXPin, TXPin);
  delay(800);

  if (send("AT", 2000).indexOf("OK") == -1) {
    modem.flush();
    while (modem.available()) modem.read();
    delay(1100);
    modem.print("+++");
    modem.flush();
    delay(1100);
    if (send("AT", 2500).indexOf("OK") == -1) {
      Serial0.println("modem unreachable even after '+++'. Stopping.");
      return;
    }
    Serial0.println("escaped PPP data mode");
  }
  send("ATE0", 1000);

  Serial0.println("");
  Serial0.println("--- 0. identify ---");
  step("ATI",                  "ATI", 3000);
  step("GCAP",                 "AT+GCAP", 2000);
  step("CEER (last error)",    "AT+CEER", 2000);

  Serial0.println("");
  Serial0.println("--- 1. bearer ---");
  step("CPIN", "AT+CPIN?", 2000);
  step("CSQ",  "AT+CSQ", 2000);
  step("CGATT","AT+CGATT?", 2000);
  step("CGACT","AT+CGACT?", 2000);
  step("CGCONTRDP=1", "AT+CGCONTRDP=1", 3000);

  Serial0.println("");
  Serial0.println("--- 2. HTTP stack via raw IP (no DNS dependency) ---");
  step("HTTPINIT", "AT+HTTPINIT", 5000);
  step("PARA URL  1.1.1.1", "AT+HTTPPARA=\"URL\",\"http://1.1.1.1\"", 3000);
  step("ACTION GET",        "AT+HTTPACTION=0", 30000);
  step("HTTPREAD",          "AT+HTTPREAD", 10000);
  step("HTTPTERM",          "AT+HTTPTERM", 3000);

  Serial0.println("");
  Serial0.println("--- 3. legacy socket stack re-check ---");
  step("CIPSTATUS", "AT+CIPSTATUS", 3000);

  Serial0.println("");
  Serial0.println("done.");
}

void loop() {
  static unsigned long last = 0;
  if (millis() - last > 6000) {
    last = millis();
    Serial0.println("---- diagnostic complete, idle");
  }
  delay(100);
}