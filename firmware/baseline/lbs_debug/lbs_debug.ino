/* LBS diagnostic probe.
 *
 * Thread: every sketch since 09-30 returns +CLBS: 10 (close network error)
 * even with +CSQ 31 and a valid PDP IP, while on 09-29 the same lbsmatrix
 * flow returned +CLBS: 0,<lon>,<lat>,<acc>. A ghost active PDP context 8
 * (never created by any of our commands) has been present on every failing
 * run via AT+CGACT?.
 *
 * Per the A76XX Series LBS Application Note V1.04, ret_code 10 == "Close
 * network error": a module-side transport failure in the LBS engine's own
 * data path. The A76XX exposes only AT+CLBS (no AT+CLBSCFG server override).
 *
 * This build:
 *  1. tears down/deletes any non-1 context, re-establishes a clean context 1;
 *  2. re-registers via CFUN=0/1;
 *  3. probes the modem-INTERNAL socket stack (AT+NETOPEN / AT+CIPOPEN) to a
 *     public host, isolating "modem data plane down" from "LBS server / carrier";
 *  4. sweeps CLBS default-cid (per the app-note example) and mode 4.
 * Nothing here changes the modem's persistent configuration.
 */

#include <Arduino.h>

#define MODEM_RX_PIN  17
#define MODEM_TX_PIN  18
#define MODEM_PWR_PIN 21
#define MODEM_PWRKEY  42
#define MODEM_BAUD    115200

HardwareSerial ss(1);
static size_t ln = 0;

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
  Serial0.print(">>> "); Serial0.println(cmd); Serial0.flush();
  unsigned long deadline = millis() + wait;
  String acc;
  char l[200];
  while (millis() < deadline) {
    if (!readLine(l, sizeof(l), deadline)) break;
    Serial0.print("  "); Serial0.println(l); Serial0.flush();
    acc += l; acc += '\n';
    if (acc.indexOf("ERROR") >= 0) break;
    if (acc.indexOf("+CLBS:") >= 0 && l[0] == '+') break;
    if (stopOnOk && acc.indexOf("OK") >= 0) break;
  }
  if (acc.length() == 0) { Serial0.println("  <no response>"); Serial0.flush(); }
  return acc;
}

static void tryCLBS(const char *cmd, const char *label) {
  Serial0.print("MARK-TRY-"); Serial0.println(label); Serial0.flush();
  String r = sendAT(cmd, 150000, false);
  Serial0.print("VERDICT "); Serial0.print(label);
  Serial0.print(" => "); Serial0.println(r.indexOf("+CLBS: 0,") >= 0 ? "SUCCESS" : "fail");
  Serial0.flush();
}

void setup() {
  Serial0.begin(115200);
  Serial0.println("MARK-START"); Serial0.flush();
  delay(1500);

  pinMode(MODEM_PWR_PIN, OUTPUT);
  digitalWrite(MODEM_PWR_PIN, HIGH);
  delay(500);
  pinMode(MODEM_PWRKEY, OUTPUT);
  /* true cold power-cycle of the modem: long PWRKEY hold powers it OFF, THEN
   * the standard ON pulse. Rules out a wedged long-lived LBS/PPP session from
   * prior churn. */
  digitalWrite(MODEM_PWRKEY, HIGH); delay(100);
  digitalWrite(MODEM_PWRKEY, LOW);  delay(2200);
  digitalWrite(MODEM_PWRKEY, HIGH); delay(3000);   /* OFF confirmed */
  Serial0.println("MARK-PWROFF"); Serial0.flush();
  digitalWrite(MODEM_PWRKEY, HIGH); delay(100);
  digitalWrite(MODEM_PWRKEY, LOW);  delay(1200);
  digitalWrite(MODEM_PWRKEY, HIGH); delay(5000);   /* ON */
  Serial0.println("MARK-PWRKEY-DONE"); Serial0.flush();

  ss.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  delay(1000);
  for (int i = 0; i < 6; i++) {
    if (sendAT("AT", 3000).indexOf("OK") >= 0) break;
    delay(1000);
  }
  Serial0.println("MARK-AT-SYNCED"); Serial0.flush();

  /* the exact byte-for-byte flow of the committed lbsmatrix sketch, which is
   * what PASSED on 09-29 (TEST-302, +CLBS: 0,18.481703,73.897415,550). No
   * context deletion, no CFUN churn, no socket probes. If this reproduces
   * +CLBS: 10 after a true modem cold power-cycle, the LBS server/service is
   * the failing boundary, not this firmware or the modem's runtime state. */
  sendAT("AT+CGNSSTST=0", 3000);
  sendAT("AT+CGNSSPWR=0", 3000);
  sendAT("AT+CSQ", 5000);
  sendAT("AT+CREG?", 5000);
  sendAT("AT+CGREG?", 5000);
  sendAT("AT+CEREG?", 5000);
  sendAT("AT+COPS?", 8000);

  /* SIMEI, exactly as lbsmatrix/position_service do it */
  String imei = sendAT("AT+CGSN", 5000);
  String digits;
  for (unsigned long i = 0; i < imei.length(); i++) {
    char c = imei[i];
    if (c >= '0' && c <= '9') digits += c;
  }
  Serial0.print("IMEI len="); Serial0.println(digits.length()); Serial0.flush();
  if (digits.length() == 15) {
    sendAT(("AT+SIMEI=" + digits).c_str(), 8000);
    sendAT("AT+SIMEI?", 8000);
  }

  /* data bearer, exactly as lbsmatrix does it */
  sendAT("AT+CNMP=38", 5000);
  sendAT("AT+CGDCONT=1,\"IP\",\"airtelgprs.com\"", 5000);
  sendAT("AT+CGATT=1", 8000);
  delay(6000);
  sendAT("AT+CGACT=1,1", 15000);
  delay(6000);
  sendAT("AT+CGACT?", 8000);
  sendAT("AT+CGPADDR=1", 8000);
  Serial0.println("MARK-BEARER-UP"); Serial0.flush();

  tryCLBS("AT+CLBS=1,1", "mode1-cid1");
  delay(3000);
  tryCLBS("AT+CLBS=9,1", "mode9-cid1");
  delay(3000);
  tryCLBS("AT+CLBS=2,1", "mode2-cid1");
  Serial0.println("MARK-LBS-DONE"); Serial0.flush();
}

void loop() { delay(1000); }