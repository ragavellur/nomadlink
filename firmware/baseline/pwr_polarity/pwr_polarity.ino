/* Determine the true polarity of the modem power-enable line, GPIO21.
 *
 * Every recovery attempt so far assumed GPIO21 HIGH means "modem powered on",
 * because that is the pattern in the other baseline sketches. That assumption
 * was never tested. If the line is actually active-low, then every "power on"
 * sequence written so far has been holding the modem OFF, and every "power
 * cycle" has been holding it off for the whole cycle.
 *
 * This walks the full 2x2 matrix of PWR level x PWRKEY pulse, and for each
 * combination reports whether the modem answers AT. The result settles what
 * BUG-005 actually is: a latched-off module, or a wrong power polarity.
 *
 * Order matters. Combinations are tried from least to most disruptive, and
 * PWR is left HIGH at the end since that is the assumed-safe state.
 */

#include <Arduino.h>

#define MODEM_TX_PIN  18
#define MODEM_RX_PIN  17
#define MODEM_PWR_PIN 21
#define MODEM_PWRKEY  42

HardwareSerial ss(1);

static bool tryAT(unsigned long perTry, int tries) {
  for (int i = 0; i < tries; i++) {
    ss.print("AT\r\n");
    unsigned long t0 = millis();
    String got = "";
    while (millis() - t0 < perTry) {
      while (ss.available()) got += (char)ss.read();
      if (got.indexOf("OK") != -1) return true;
      yield();
    }
  }
  return false;
}

static void setPwr(int level) {
  digitalWrite(MODEM_PWR_PIN, level ? HIGH : LOW);
  delay(2500);
}

static void pulse() {
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, HIGH); delay(100);
  digitalWrite(MODEM_PWRKEY, LOW);  delay(1200);
  digitalWrite(MODEM_PWRKEY, HIGH); delay(5000);
}

static bool found = false;

static void attempt(const char *label, int pwrLevel, bool sendPulse) {
  if (found) return;
  Serial0.printf("\n--- %s (PWR=%s, PWRKEY=%s) ---\n", label,
                 pwrLevel ? "HIGH" : "LOW", sendPulse ? "pulse" : "none");
  Serial0.flush();

  setPwr(pwrLevel);
  if (sendPulse) pulse();

  ss.end();
  ss.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  delay(500);

  bool ok = tryAT(2000, 5);
  Serial0.printf("    AT -> %s\n", ok ? "OK" : "silent");
  Serial0.flush();
  if (ok) { found = true; Serial0.printf("  >>> ANSWERS with PWR=%s, PWRKEY=%s\n",
                                        pwrLevel ? "HIGH" : "LOW",
                                        sendPulse ? "pulse" : "none"); }
}

void setup() {
  Serial0.begin(115200);
  Serial0.println("\n========== MODEM POWER POLARITY PROBE ==========");
  Serial0.flush();
  delay(1500);

  pinMode(MODEM_PWR_PIN, OUTPUT);
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, HIGH);

  /* The historically successful combination first, so if it now works the
   * module was merely latched and has recovered on its own. */
  attempt("baseline HIGH+pulse", 1, true);
  attempt("HIGH, no pulse",      1, false);
  attempt("LOW+pulse",           0, true);
  attempt("LOW, no pulse",       0, false);

  Serial0.println("\n==============================================");
  if (found) {
    Serial0.println("RESULT MODEM_POWER_POLARITY REACHABLE");
    Serial0.println("  Record the combination above as the correct power sequence.");
  } else {
    Serial0.println("RESULT MODEM_POWER_POLARITY UNREACHABLE");
    Serial0.println("  No combination of GPIO21/PWRKEY reaches the modem.");
    Serial0.println("  => the module is genuinely latched off; USB must be removed.");
  }
  Serial0.println("========== POLARITY PROBE COMPLETE ==========");
  Serial0.flush();

  /* Leave the board in the assumed-good state. */
  digitalWrite(MODEM_PWR_PIN, HIGH);
}

void loop() { delay(1000); }
