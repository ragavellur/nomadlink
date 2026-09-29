/* SMS test - ESP32-S3-A7670E-4G
 *
 * Target: <redacted>. Previous run returned +CMGS: 1, but that only means the
 * message was handed to the network, not that it was delivered. This run
 * enables delivery notifications and waits for the delivery report, then
 * reads the sent box back to confirm.
 *
 * Modem access: GPIO33 HIGH and held, UART RX=17 TX=18 @115200.
 */

#include <Arduino.h>

static const int PIN_MODEM_CTRL = 33;
static const int PIN_RX = 17;
static const int PIN_TX = 18;
static const uint32_t BAUD = 115200;
// Set your own target number below. It is deliberately not committed: a
// personal phone number is a device/owner identifier and this file is tracked.
// Put a real number in a local, gitignored copy, or edit it only for a local flash.
static const char *TEL = "0000000000";  // set locally; do not commit a real number

static HardwareSerial &ser() { return Serial1; }

static void drain() { while (ser().available()) ser().read(); }

static String at(const char *cmd, uint32_t waitMs = 5000)
{
  drain();
  delay(150);
  ser().print(cmd);
  ser().print("\r\n");
  ser().flush();
  String out;
  uint32_t t0 = millis();
  while (millis() - t0 < waitMs) {
    while (ser().available()) {
      char c = (char)ser().read();
      if (c == '\n') out += "\n";
      else if (c != '\r') out += c;
    }
    if (out.indexOf("OK") >= 0 || out.indexOf("ERROR") >= 0) {
      delay(300);
      while (ser().available()) { char c = (char)ser().read(); if (c != '\r') out += c; }
      break;
    }
    delay(10);
  }
  out.trim();
  return out;
}

static void show(const char *label, const char *cmd, uint32_t wait = 5000)
{
  String r = at(cmd, wait);
  r.replace("\n", " | ");
  Serial0.println(String(label) + " -> " + r);
}

/* Send one text SMS. Returns the +CMGS reference, or empty on failure. */
static String sendSms(const char *body)
{
  drain();
  delay(250);
  ser().print("AT+CMGS=\"");
  ser().print(TEL);
  ser().print("\"\r\n");
  ser().flush();

  uint32_t t0 = millis();
  bool prompted = false;
  while (millis() - t0 < 12000) {
    if (ser().available()) {
      char c = (char)ser().read();
      Serial0.print(c);
      if (c == '>') { prompted = true; break; }
    }
    delay(10);
  }
  Serial0.println();
  if (!prompted) return "";

  delay(250);
  ser().print(body);
  ser().print((char)0x1A);
  ser().flush();

  String acc;
  t0 = millis();
  while (millis() - t0 < 30000) {
    while (ser().available()) {
      char c = (char)ser().read();
      if (c != '\r') Serial0.print(c);
      if (c == '\n') Serial0.println();
      acc += c;
    }
    delay(20);
  }
  Serial0.println();
  return acc;
}

void setup()
{
  Serial0.begin(115200);
  delay(2500);
  Serial0.println("\n========== SMS TEST ==========");
  Serial0.println("Target: " + String(TEL));

  pinMode(PIN_MODEM_CTRL, OUTPUT);
  digitalWrite(PIN_MODEM_CTRL, HIGH);
  ser().begin(BAUD, SERIAL_8N1, PIN_RX, PIN_TX);
  delay(2500);
  drain();

  Serial0.println("\n-- setup --");
  show("AT", "AT", 4000);
  show("CPIN?", "AT+CPIN?", 4000);
  show("CREG? GSM reg", "AT+CREG?", 6000);
  show("CSCA? SMSC", "AT+CSCA?", 4000);
  show("CMGF=1 text", "AT+CMGF=1", 4000);
  show("CSCS=GSM", "AT+CSCS=\"GSM\"", 4000);
  show("CNMI delivery", "AT+CNMI=2,1", 4000);
  show("CSMP=17,167,0,0", "AT+CSMP=17,167,0,0", 4000);

  /* ---- message 1: short ---- */
  Serial0.println("\n-- message 1 (short) --");
  String r1 = sendSms("ESP32-S3-A7670E-4G SMS test 1");
  Serial0.println("  MSG1 -> " + String(r1.indexOf("+CMGS: ") >= 0 ? "SUBMITTED " : "NO SUBMIT ") + r1);

  /* ---- message 2: timestamped ---- */
  Serial0.println("\n-- message 2 (timestamped) --");
  String body = "ESP32-S3-A7670E-4G sent at ";
  body += String(millis() / 1000);
  body += "s uptime";
  String r2 = sendSms(body.c_str());
  Serial0.println("  MSG2 -> " + String(r2.indexOf("+CMGS: ") >= 0 ? "SUBMITTED " : "NO SUBMIT ") + r2);

  /* ---- wait for delivery report ---- */
  Serial0.println("\n-- waiting up to 90s for delivery report --");
  uint32_t t0 = millis();
  int reports = 0;
  char line[200]; int llen = 0;
  while (millis() - t0 < 90000) {
    while (ser().available()) {
      char c = (char)ser().read();
      if (c == '\n') {
        line[llen] = 0;
        if (llen > 0) {
          if (strstr(line, "CDS") || strstr(line, "CDSI")) {
            Serial0.println("  DELIVERY> " + String(line));
            reports++;
          }
        }
        llen = 0;
      } else if (c != '\r' && llen < 199) {
        line[llen++] = c;
      }
    }
    delay(10);
  }
  Serial0.println("  delivery reports seen: " + String(reports));

  /* ---- read sent box back ---- */
  Serial0.println("\n-- sent box (CMGL=\"SENT\") --");
  show("CMGL SENT", "AT+CMGL=\"SENT\"", 8000);

  Serial0.println("\nSMS-DONE");
  Serial0.flush();
}

void loop() { delay(1000); }
