/* Voice + SMS test - ESP32-S3-A7670E-4G
 *
 * Target number: <redacted>
 *   Phase 1: voice capability / audio path
 *   Phase 2: send an SMS, capture the +CMGS reference
 *   Phase 3: place a voice call, hold it open, poll call state, hang up
 *
 * Known-good modem access: GPIO33 driven HIGH and held, UART RX=17 TX=18 @115200.
 * SMS: AT+CMGF=1 then AT+CMGS="<num>", body, Ctrl+Z (0x1A).
 * Voice: ATD<num>; then AT+CLCC? to poll, ATH to hang up.
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

void setup()
{
  Serial0.begin(115200);
  delay(2500);
  Serial0.println("\n========== VOICE + SMS TEST ==========");
  Serial0.println("Target: " + String(TEL));

  pinMode(PIN_MODEM_CTRL, OUTPUT);
  digitalWrite(PIN_MODEM_CTRL, HIGH);
  ser().begin(BAUD, SERIAL_8N1, PIN_RX, PIN_TX);
  delay(2500);
  drain();

  /* ---------------- Phase 1 ---------------- */
  Serial0.println("\n-- Phase 1: voice capability --");
  show("AT", "AT", 4000);
  show("CPIN?", "AT+CPIN?", 4000);
  show("CPAS? call status", "AT+CPAS?", 5000);
  show("CVSD? voice supp", "AT+CVSD?", 4000);
  show("CVO? voice op", "AT+CVO?", 4000);
  show("CEREG? LTE reg", "AT+CEREG?", 5000);
  show("CREG? GSM reg", "AT+CREG?", 5000);
  show("CLV? spk volume", "AT+CLV?", 4000);
  show("CMUT? mute", "AT+CMUT?", 4000);
  show("CNMP? mode", "AT+CNMP?", 4000);

  /* ---------------- Phase 2: SMS ---------------- */
  Serial0.println("\n-- Phase 2: SMS --");
  show("CSCA? SMSC", "AT+CSCA?", 4000);
  show("CMGF=1 text", "AT+CMGF=1", 4000);
  show("CSCS=GSM", "AT+CSCS=\"GSM\"", 4000);

  Serial0.println("\n  dialling SMS to " + String(TEL));
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

  if (!prompted) {
    Serial0.println("  RESULT SMS  FAIL - no '>' prompt, nothing sent");
  } else {
    delay(250);
    ser().print("Test from ESP32-S3-A7670E-4G");
    ser().print((char)0x1A);            /* Ctrl+Z sends */
    ser().flush();
    Serial0.println("  body sent + Ctrl+Z, waiting for +CMGS ref...");
    t0 = millis();
    while (millis() - t0 < 50000) {
      while (ser().available()) {
        char c = (char)ser().read();
        if (c != '\r') Serial0.print(c);
        if (c == '\n') Serial0.println();
      }
      delay(20);
    }
    Serial0.println();
  }

  /* ---------------- Phase 3: call ---------------- */
  Serial0.println("\n-- Phase 3: voice call --");
  show("CLCC? pre-call", "AT+CLCC?", 5000);
  drain();
  delay(300);
  ser().print("ATD");
  ser().print(TEL);
  ser().print(";\r\n");
  ser().flush();
  Serial0.println("  sent ATD" + String(TEL) + ";");

  t0 = millis();
  bool begun = false, answered = false;
  uint32_t lastPoll = millis();
  char line[192];
  int llen = 0;

  while (millis() - t0 < 100000) {
    while (ser().available()) {
      char c = (char)ser().read();
      if (c == '\n') {
        line[llen] = 0;
        if (llen > 0) {
          Serial0.println("  CALL> " + String(line));
          if (strstr(line, "VOICE CALL: BEGIN")) begun = true;
          if (strstr(line, "NO CARRIER")) { Serial0.println("  !! NO CARRIER"); begun = false; }
          if (strstr(line, "BUSY"))       { Serial0.println("  !! BUSY"); }
        }
        llen = 0;
      } else if (c != '\r' && llen < 191) {
        line[llen++] = c;
      }
    }

    if (millis() - lastPoll > 15000) {
      lastPoll = millis();
      String st = at("AT+CLCC?", 5000);
      st.replace("\n", " | ");
      Serial0.println("  [poll] CLCC=" + st);
      if (st.indexOf(",2") >= 0) answered = true;   /* 2 = active */
      if (st.indexOf(",4") >= 0) break;             /* 4 = held   */
      if (!begun && millis() - t0 > 60000) break;   /* never connected */
    }
    delay(10);
  }

  Serial0.println("  connected=" + String(begun ? "yes" : "no") +
                  "  answered=" + String(answered ? "yes" : "no"));
  Serial0.println("  hanging up");
  show("ATH", "AT+ATH", 12000);
  delay(2500);
  show("CLCC? post-call", "AT+CLCC?", 5000);

  Serial0.println("\nVOICE-DONE");
  Serial0.flush();
}

void loop() { delay(1000); }
