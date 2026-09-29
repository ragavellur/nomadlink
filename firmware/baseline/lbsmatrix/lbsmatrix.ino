/* A7670E LBS matrix with patch antenna connected.
 *
 * AT+CLBS=? -> +CLBS: (1,2,3,4,9),(1-15),(-180..180),(-90..90),(0,1)
 * So: AT+CLBS=<mode>,<cid>
 * Earlier attempts used <mode> only, defaulting cid to an unusable value.
 * SIMEI is populated from AT+CGSN before querying.
 */

#include <Arduino.h>
#include <string.h>

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

static String sendAT(const char *cmd, unsigned long wait) {
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
  }
  if (acc.length() == 0) { Serial0.println("  <no response>"); Serial0.flush(); }
  return acc;
}

static void tryCLBS(const char *cmd, const char *label) {
  Serial0.print("MARK-TRY-"); Serial0.println(label); Serial0.flush();
  String r = sendAT(cmd, 150000);
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
  digitalWrite(MODEM_PWRKEY, HIGH); delay(100);
  digitalWrite(MODEM_PWRKEY, LOW);  delay(1200);
  digitalWrite(MODEM_PWRKEY, HIGH); delay(5000);
  Serial0.println("MARK-PWRKEY-DONE"); Serial0.flush();

  ss.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  delay(1000);
  for (int i = 0; i < 6; i++) {
    if (sendAT("AT", 3000).indexOf("OK") >= 0) break;
    delay(1000);
  }
  Serial0.println("MARK-AT-SYNCED"); Serial0.flush();

  sendAT("AT+CGNSSTST=0", 3000);
  sendAT("AT+CGNSSPWR=0", 3000);

  /* signal quality with the patch antenna attached */
  sendAT("AT+CSQ", 5000);
  sendAT("AT+CREG?", 5000);
  sendAT("AT+CGREG?", 5000);
  sendAT("AT+CEREG?", 5000);
  sendAT("AT+COPS?", 8000);

  /* SIMEI */
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

  /* data bearer */
  sendAT("AT+CNMP=38", 5000);
  sendAT("AT+CGDCONT=1,\"IP\",\"airtelgprs.com\"", 5000);
  sendAT("AT+CGATT=1", 8000);
  delay(6000);
  sendAT("AT+CGACT=1,1", 15000);
  delay(6000);
  sendAT("AT+CGACT?", 8000);
  sendAT("AT+CGPADDR=1", 8000);
  Serial0.println("MARK-BEARER-UP"); Serial0.flush();

  /* the matrix the capability query pointed at */
  tryCLBS("AT+CLBS=1,1", "mode1-cid1");
  delay(3000);
  tryCLBS("AT+CLBS=9,1", "mode9-cid1");
  delay(3000);
  tryCLBS("AT+CLBS=2,1", "mode2-cid1");

  Serial0.println("MARK-LBS-DONE"); Serial0.flush();
}

void loop() { delay(1000); }
