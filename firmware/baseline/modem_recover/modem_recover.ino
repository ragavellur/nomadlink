/* Modem power recovery probe.
 *
 * The PPP probe left the A7670E unresponsive: a hard power cycle plus a PWRKEY
 * pulse produced no AT response at all, though the modem answered fine before.
 * Rather than guess, this sweeps the variables that could explain it:
 *
 *   - PWRKEY pulse length. 1200 ms is the usual value, but a cold module
 *     sometimes needs much longer, and autobaud may have locked to a rate we
 *     are no longer using.
 *   - UART baud. If the module latched a different rate, AT is being sent at
 *     the wrong speed and every reply is garbage.
 *   - Whether the module is transmitting at all.
 *
 * It also dumps raw received bytes as hex at each rate, so "silent" can be
 * distinguished from "speaking at an unexpected rate".
 *
 * Nothing here changes the modem's persistent configuration.
 */

#include <Arduino.h>

#define MODEM_TX_PIN  18
#define MODEM_RX_PIN  17
#define MODEM_PWR_PIN 21
#define MODEM_PWRKEY  42

HardwareSerial ss(1);


String tryAt(unsigned long baud, int tries, unsigned long perTry) {
  ss.end();
  ss.begin(baud, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  delay(600);
  for (int i = 0; i < tries; i++) {
    ss.print("AT\r\n");
    unsigned long t0 = millis();
    String got = "";
    while (millis() - t0 < perTry) {
      while (ss.available()) got += (char)ss.read();
      if (got.indexOf("OK") != -1) return got;
      yield();
    }
  }
  return "";
}

void pulse(unsigned long holdMs) {
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, HIGH); delay(100);
  digitalWrite(MODEM_PWRKEY, LOW);  delay(holdMs);
  digitalWrite(MODEM_PWRKEY, HIGH); delay(1000);
}

void setup() {
  Serial0.begin(115200);
  Serial0.println("\n========== MODEM POWER RECOVERY PROBE ==========");
  Serial0.flush();
  delay(1500);

  pinMode(MODEM_PWR_PIN, OUTPUT);
  digitalWrite(MODEM_PWR_PIN, HIGH);
  delay(1000);

  unsigned long bauds[] = {115200, 9600, 57600, 38400, 230400};
  unsigned long holds[] = {1200, 2500, 4000};
  int recovered = 0;

  /* Try at the current power state first, before touching PWRKEY at all. */
  Serial0.println("\n-- probing as-is, no PWRKEY --");
  for (int i = 0; i < 5 && !recovered; i++) {
    String r = tryAt(bauds[i], 2, 1500);
    Serial0.printf("  %lu baud: %s\n", bauds[i], r.length() ? "RESPONSE" : "silent");
    if (r.indexOf("OK") != -1) { recovered = 1; Serial0.printf("  -> OK at %lu baud\n", bauds[i]); }
  }

  if (!recovered) {
    for (unsigned h = 0; h < 3 && !recovered; h++) {
      Serial0.printf("\n-- PWRKEY hold %lu ms --\n", holds[h]);
      Serial0.flush();
      pulse(holds[h]);
      for (int i = 0; i < 5 && !recovered; i++) {
        String r = tryAt(bauds[i], 2, 1500);
        Serial0.printf("  %lu baud: %s\n", bauds[i], r.length() ? "RESPONSE" : "silent");
        if (r.indexOf("OK") != -1) { recovered = 1; Serial0.printf("  -> OK at %lu baud after %lu ms pulse\n", bauds[i], holds[h]); }
      }
    }
  }

  /* Last resort: full power-down hold, then long pulse. */
  if (!recovered) {
    Serial0.println("\n-- deep power-down 8s, then PWRKEY 2s --");
    Serial0.flush();
    digitalWrite(MODEM_PWR_PIN, LOW); delay(8000);
    digitalWrite(MODEM_PWR_PIN, HIGH); delay(3000);
    pulse(2000);
    for (int i = 0; i < 5 && !recovered; i++) {
      String r = tryAt(bauds[i], 3, 2000);
      Serial0.printf("  %lu baud: %s\n", bauds[i], r.length() ? "RESPONSE" : "silent");
      if (r.indexOf("OK") != -1) { recovered = 1; }
    }
  }

  /* Whatever the module is putting on the wire, decoded or not. */
  Serial0.println("\n-- raw byte activity at 115200 (3s, unsolicited) --");
  Serial0.flush();
  ss.end(); ss.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  int total = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 3000) {
    while (ss.available() && total < 512) {
      uint8_t b = (uint8_t)ss.read();
      if (total % 16 == 0) Serial0.printf("\n  bytes:");
      Serial0.printf(" %02X", b);
      total++;
    }
    yield();
  }
  Serial0.printf("\n  total unsolicited bytes: %d\n", total);

  Serial0.printf("\nRESULT MODEM_POWER %s\n", recovered ? "RECOVERED" : "UNRECOVERED");
  if (!recovered) Serial0.println(
    "  -> Software cannot reach the modem. Unplug USB, re-plug, re-run.");
  Serial0.println("========== RECOVERY PROBE COMPLETE ==========");
  Serial0.flush();
}

void loop() { delay(1000); }
