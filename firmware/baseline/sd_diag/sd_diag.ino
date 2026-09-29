/* SD card diagnostic only - ESP32-S3-A7670E-4G
 *
 * Core 3.3.11 API: SD_MMC.setPins(clk,cmd,d0) then
 *   begin(mountpoint, mode1bit, format_if_mount_failed, freq_kHz, maxOpenFiles)
 * The 4th argument is the SDMMC clock in kHz, NOT a block size.
 *
 * ESP-IDF's own SDMMC error logs also reach UART0, which tells us whether the
 * card was detected at all versus failing to mount for filesystem reasons.
 */

#include <Arduino.h>
#include "SD_MMC.h"

#define V1_CLK 5
#define V1_CMD 4
#define V1_D0  6
#define SD_CD  46

static bool mounted = false;

static void attempt(bool one_bit, bool fmt, int clk, int cmd, int d0,
                    int freq_khz, const char *label)
{
  Serial0.printf("\n--- %s ---\n", label);
  Serial0.printf("  pins: %s  1bit=%d  fmt=%d  freq=%d kHz\n",
                 (clk < 0) ? "library default" : "explicit", one_bit, fmt, freq_khz);
  if (clk >= 0) SD_MMC.setPins(clk, cmd, d0);
  mounted = SD_MMC.begin("/sdcard", one_bit, fmt, freq_khz, 5);
  if (!mounted) {
    Serial0.println("  mount FAILED");
    return;
  }
  uint64_t sz = SD_MMC.cardSize();
  Serial0.printf("  mount OK, size=%llu bytes (%.2f MB)\n",
                 (unsigned long long)sz, sz / 1048576.0);
  Serial0.printf("  total=%llu MB  used=%llu MB\n",
                 (unsigned long long)(SD_MMC.totalBytes() / 1048576),
                 (unsigned long long)(SD_MMC.usedBytes() / 1048576));
}

static void finish()
{
  Serial0.println("\n--- write/read test ---");
  File f = SD_MMC.open("/selftest.txt", FILE_WRITE);
  if (!f) { Serial0.println("RESULT SDCARD   FAIL  mounted but file not writable"); Serial0.println("SDDIAG-DONE"); Serial0.flush(); return; }
  f.println("ESP32-S3-A7670E-4G sd test");
  f.close();

  char buf[96] = {0};
  File r = SD_MMC.open("/selftest.txt", FILE_READ);
  if (!r) { Serial0.println("RESULT SDCARD   FAIL  wrote but could not reopen for reading"); Serial0.println("SDDIAG-DONE"); Serial0.flush(); return; }
  size_t n = r.readBytesUntil('\n', buf, sizeof(buf) - 1);
  r.close();
  SD_MMC.remove("/selftest.txt");

  bool ok = (n > 0) && strncmp(buf, "ESP32-S3-A7670E-4G", 18) == 0;
  Serial0.printf("  read back %u bytes: \"%s\"\n", (unsigned)n, buf);
  Serial0.printf("RESULT SDCARD   %s  write+read round-trip %s\n",
                 ok ? "PASS" : "FAIL", ok ? "ok" : "MISMATCH");
  Serial0.println("SDDIAG-DONE");
  Serial0.flush();
}

void setup()
{
  Serial0.begin(115200);
  delay(2500);
  Serial0.println("\n========== SD CARD DIAGNOSTIC ==========");
  Serial0.printf("Free heap: %u\n", ESP.getFreeHeap());
  pinMode(SD_CD, INPUT_PULLUP);
  Serial0.printf("Card detect pin %d: %s (informational only)\n",
                 SD_CD, digitalRead(SD_CD) == LOW ? "LOW" : "HIGH");

  /* Try the guide's V1 pins, then library defaults, without formatting first. */
  attempt(true,  false, V1_CLK, V1_CMD, V1_D0, 20000, "Pass 1: V1 pins, 1-bit, no format");
  if (!mounted) attempt(false, false, V1_CLK, V1_CMD, V1_D0, 20000, "Pass 2: V1 pins, 4-bit, no format");
  if (!mounted) attempt(true,  false, -1, -1, -1, 20000, "Pass 3: default pins, 1-bit, no format");
  if (!mounted) attempt(false, false, -1, -1, -1, 20000, "Pass 4: default pins, 4-bit, no format");

  /* Nothing mounted: allow the driver to format the card to FAT. */
  if (!mounted) attempt(true,  true, V1_CLK, V1_CMD, V1_D0, 20000, "Pass 5: V1 pins, 1-bit, formatOnFail");
  if (!mounted) attempt(false, true, V1_CLK, V1_CMD, V1_D0, 20000, "Pass 6: V1 pins, 4-bit, formatOnFail");
  if (!mounted) attempt(true,  true, -1, -1, -1, 20000, "Pass 7: default pins, 1-bit, formatOnFail");
  if (!mounted) attempt(false, true, -1, -1, -1, 20000, "Pass 8: default pins, 4-bit, formatOnFail");

  if (mounted) { finish(); return; }
  Serial0.println("\nRESULT SDCARD   FAIL  card did not mount in any configuration");
  Serial0.println("SDDIAG-DONE");
  Serial0.flush();
}

void loop() { delay(1000); }
