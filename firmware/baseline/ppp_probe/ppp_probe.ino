/* TASK-020 / TEST-020 - can PPP produce a real IP interface on the Airtel SIM?
 *
 * WHY THIS EXPERIMENT
 * The earlier PPP attempt failed with "+CGREG: 0,3" on a Jio SIM. 0,3 is
 * "registration denied" - that is a NETWORK/SIM symptom, not evidence that PPP
 * itself is broken on this modem. So the first question is not "does PPP work"
 * but "is this SIM allowed on this network at all".
 *
 * The stages below are independent, so the run is informative even if a later
 * stage is a dead end:
 *
 *   S1  Identify the modem, SIM and network.
 *   S2  Registration: CEREG (LTE) and CGREG. THIS is what failed before.
 *   S3  Data bearer: attach, read APN, activate PDP context, read the IP.
 *       This is the path already proven to work, so it should pass. If it does
 *       NOT, that is a bigger problem than PPP.
 *   S4  PPP capability probe. The A7670E is wired to us over UART, but
 *       esp_modem's A7670 PPP path is USB-only, so the stock path does not
 *       apply. This stage asks the modem directly what it supports.
 *   S5  PPP dial attempt, then hang up cleanly. Left in data mode? We abort.
 *
 * NOTHING here assumes the answer. Every stage prints raw responses so the
 * result can be judged from evidence rather than from a summary line.
 *
 * Modem: UART GPIO17(RX)/18(TX), PWR GPIO21, PWRKEY GPIO42, 115200.
 * Console: Serial0 (UART0 -> CH9102). "Serial" is the modem's USB, not the host.
 */

#include <Arduino.h>

#define MODEM_TX_PIN  18
#define MODEM_RX_PIN  17
#define MODEM_PWR_PIN 21
#define MODEM_PWRKEY  42
#define MODEM_BAUD    115200

#define APN "airtelgprs.com"

HardwareSerial ss(1);
String rev;

void SentSerial(const char *s) {
  while (ss.available()) ss.read();
  for (size_t i = 0; i < strlen(s); i++) { ss.write(s[i]); delay(2); }
  ss.write('\r'); ss.write('\n');
}

/* Send a command and collect until OK/ERROR, or until the timeout. */
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

/* Send a command whose reply is a URC or a prompt, not OK/ERROR. */
String sendRaw(const char *s, unsigned long t) {
  SentSerial(s);
  unsigned long start = millis();
  rev = "";
  while (millis() - start < t) {
    while (ss.available()) { char c = ss.read(); rev += c; }
    yield();
  }
  return rev;
}

void dump(const char *label, const char *cmd, String resp) {
  Serial0.printf("\n--- %s\n    > %s\n", label, cmd);
  String one = resp; one.replace("\r\n", " | "); one.replace("\n", " | ");
  one.trim();
  Serial0.printf("    < %s\n", one.length() ? one.c_str() : "(no response)");
  Serial0.flush();
}

/* The PWRKEY pulse is NOT idempotent. On an already-on modem it ends a call or
 * aborts a data session; only on an off modem does it power on. After a run that
 * dialed, a blind pulse would therefore hang up and leave us with a dead modem,
 * which is exactly what happened on the first re-run of this sketch.
 *
 * So: try talking to the modem first, and only pulse if it is genuinely asleep.
 */
bool atAlive(unsigned long perTry, int tries) {
  for (int i = 0; i < tries; i++) {
    if (send("AT", perTry).indexOf("OK") != -1) return true;
    delay(400);
  }
  return false;
}

void powerOnModem() {
  pinMode(MODEM_PWR_PIN, OUTPUT);

  /* Hard power cycle first. PWRKEY cannot wake a module that has been hard
   * powered off; the PWR line has to be re-asserted before PWRKEY means
   * anything. This is the state a modem is left in if a run ends in data mode.
   */
  Serial0.println("PWR-ON: hard power cycle (PWR low 3s, then high)");
  Serial0.flush();
  digitalWrite(MODEM_PWR_PIN, LOW);
  delay(3000);
  digitalWrite(MODEM_PWR_PIN, HIGH);
  delay(2000);

  ss.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  delay(500);

  Serial0.println("PWR-ON: asserting PWR, probing before any PWRKEY pulse");
  Serial0.flush();
  if (atAlive(2000, 3)) {
    Serial0.println("PWR-ON: already powered (no PWRKEY pulse sent)");
    Serial0.flush();
    return;
  }

  Serial0.println("PWR-ON: no AT, sending PWRKEY pulse");
  Serial0.flush();
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, HIGH); delay(100);
  digitalWrite(MODEM_PWRKEY, LOW);  delay(1200);
  digitalWrite(MODEM_PWRKEY, HIGH); delay(6000);

  if (atAlive(2000, 8)) {
    Serial0.println("PWR-ON: powered by PWRKEY pulse");
  } else {
    Serial0.println("PWR-ON: STILL SILENT after hard power cycle + PWRKEY pulse");
  }
  Serial0.flush();
}

void setup() {
  Serial0.begin(115200);
  Serial0.println("\n========== TASK-020 PPP DATA PATH PROBE ==========");
  Serial0.flush();
  delay(1500);

  powerOnModem();
  Serial0.println("MODEM-POWERED"); Serial0.flush();

  if (!atAlive(2000, 3)) {
    Serial0.println("RESULT PPP_DATA_PATH FAIL - modem did not answer AT");
    Serial0.flush();
    return;
  }
  Serial0.println("MODEM-AT-OK"); Serial0.flush();

  /* ---------------- S1: identify ---------------- */
  Serial0.println("\n========== S1 IDENTIFY =========="); Serial0.flush();
  dump("firmware",   "AT+CGMR",        send("AT+CGMR", 3000));
  dump("imsi",       "AT+CGSN",        send("AT+CGSN", 3000));
  dump("iccid",      "AT+CICCID",      send("AT+CICCID", 3000));
  dump("operator",   "AT+COPS?",       send("AT+COPS?", 5000));
  dump("signal",     "AT+CSQ",         send("AT+CSQ", 3000));

  /* ---------------- S2: registration (the thing that failed) ------------- */
  Serial0.println("\n========== S2 REGISTRATION =========="); Serial0.flush();
  dump("LTE CEREG",  "AT+CEREG?",      send("AT+CEREG?", 4000));
  dump("GSM CGREG",  "AT+CGREG?",      send("AT+CGREG?", 4000));
  dump("CS CREG",    "AT+CREG?",       send("AT+CREG?", 4000));
  dump("attach",     "AT+CGATT?",      send("AT+CGATT?", 3000));

  /* ---------------- S3: data bearer ------------------------------------- */
  Serial0.println("\n========== S3 DATA BEARER =========="); Serial0.flush();
  dump("attach req", "AT+CGATT=1",     send("AT+CGATT=1", 10000));
  delay(2000);
  dump("pdp contexts", "AT+CGDCONT?",  send("AT+CGDCONT?", 4000));
  {
    /* Set context 1 explicitly, then read back what the SIM actually has. */
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", APN);
    dump("set pdp ctx 1", cmd, send(cmd, 5000));
  }
  dump("pdp readback", "AT+CGDCONT?",  send("AT+CGDCONT?", 4000));
  dump("activate 1",  "AT+CGACT=1,1",   send("AT+CGACT=1,1", 20000));
  dump("active?",     "AT+CGACT?",      send("AT+CGACT?", 4000));
  String ip = send("AT+CGPADDR=1", 5000);
  dump("our ip",      "AT+CGPADDR=1",   ip);
  {
    /* The verdict for S3. An IP here means the network gave us data on this
     * SIM, regardless of anything to do with PPP.
     *
     * NOTE: the modem returns BOTH forms depending on firmware:
     *   +CGPADDR: 1,100.77.145.7          (unquoted)
     *   +CGPADDR: 1,"100.77.145.7"        (quoted)
     * Matching only the quoted form made a working SIM report FAIL. Parse both.
     */
    bool haveIp = false;
    int p = 0;
    while ((p = ip.indexOf("+CGPADDR: 1,", p)) != -1) {
      String v = ip.substring(p + 13); v.trim();
      v = v.substring(0, v.indexOf("\r") == -1 ? v.length() : v.indexOf("\r"));
      v.replace("\"", ""); v.trim();
      /* A real address is dotted quad with every octet 0-255 and not all zero. */
      if (v.length() > 6) {
        bool ok = true; int octets = 0; int cur = 0; bool digits = false;
        for (unsigned i = 0; i <= v.length(); i++) {
          char c = (i < v.length()) ? v[i] : '.';
          if (c == '.') { if (!digits || cur > 255) ok = false; octets++; cur = 0; digits = false; }
          else if (c >= '0' && c <= '9') { cur = cur * 10 + (c - '0'); digits = true; }
          else { ok = false; }
        }
        if (ok && octets == 4 && v != "0.0.0.0" && v != "255.255.255.255") haveIp = true;
      }
      p += 13;
    }
    Serial0.printf("\nRESULT S3_DATA_BEARER %s\n", haveIp ? "PASS" : "FAIL");
    if (!haveIp) Serial0.println(
      "  -> The SIM/network did not yield a data IP. PPP cannot work without it.");
    Serial0.flush();
  }

  /* ---------------- S4: PPP capability probe ----------------------------- */
  Serial0.println("\n========== S4 PPP CAPABILITY PROBE =========="); Serial0.flush();
  Serial0.println("esp_modem's A7670 PPP path is USB-only; this board uses UART.");
  Serial0.println("Asking the modem what it actually supports:");
  Serial0.flush();
  dump("upsd support", "AT+UPSD=?",      send("AT+UPSD=?", 4000));
  dump("upsd read",    "AT+UPSD?",       send("AT+UPSD?", 4000));
  dump("cgpcont",      "AT+CGPCCNT?",    send("AT+CGPCCNT=?", 4000));
  dump("qcdrs",        "AT+QCDRS?",      send("AT+QCDRS=?", 3000));
  dump("dial support", "ATD=?",          send("ATD=?", 3000));

  /* ---------------- S5: PPP dial attempt -------------------------------- */
  Serial0.println("\n========== S5 PPP DIAL ATTEMPT =========="); Serial0.flush();
  Serial0.println("*99# is the standard PPP dial string. Watching for a prompt");
  Serial0.println("or a CONNECT, then we abort so the modem is left clean.");
  Serial0.flush();
  String dial = sendRaw("ATD*99#", 12000);
  dump("dial *99#", "ATD*99#", dial);
  {
    bool gotConnect = dial.indexOf("CONNECT") != -1;
    Serial0.printf("\nRESULT S5_PPP_DIAL %s (CONNECT seen: %s)\n",
                  gotConnect ? "POSSITIVE" : "NO-CONNECT",
                  gotConnect ? "yes" : "no");
    Serial0.flush();
  }
  /* Always abort. Leaving the modem in data mode would break every later test. */
  send("ATH", 4000);
  send("AT", 2000);
  Serial0.println("MODEM-LEFT-CLEAN");
  Serial0.flush();

  Serial0.println("\n========== PROBE COMPLETE ==========");
  Serial0.println("Interpret S1..S5 in order. S2 is the historical failure point.");
  Serial0.println("S3 passing means the network is usable; S5 decides the PPP path.");
  Serial0.flush();
}

void loop() { delay(1000); }
