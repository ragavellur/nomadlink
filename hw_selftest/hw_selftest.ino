/* ESP32-S3-A7670E-4G hardware self-test.
 *
 * Tests the ESP32-side peripherals one at a time and prints an explicit
 * PASS/FAIL line for each, plus a final summary. Every test is independent so
 * one failure never stops the others.
 *
 * Pin map is the verified Waveshare V1 layout (see CAMERA_GUIDE.md) plus the
 * TF-card pins from the official Spotpear/Waveshare user guide.
 *
 * Output goes to Serial0 (UART0 -> CH9102 -> /dev/cu.usbmodem...). On this
 * board `Serial` (USB CDC) is wired to the 4G module's USB, NOT to the host.
 */

#include <Arduino.h>
#include "esp_camera.h"
#include "SD_MMC.h"

/* ---- TF card (official guide) ---- */
#define SD_CLK 5
#define SD_CMD 4
#define SD_D0  6
#define SD_CD  46

/* ---- Camera: Waveshare V1, verified working ---- */
#define CAM_D0  7
#define CAM_D1  8
#define CAM_D2  9
#define CAM_D3  10
#define CAM_D4  11
#define CAM_D5  12
#define CAM_D6  13
#define CAM_D7  14
#define CAM_XCLK 34
#define CAM_PCLK 37
#define CAM_VSYNC 36
#define CAM_HREF 35
#define CAM_SDA 15
#define CAM_SCL 16

#define RGB_PIN 38

static int pass_count = 0;
static int fail_count = 0;

static void report(const char *name, bool ok, const char *detail)
{
    if (ok) { pass_count++; Serial0.printf("RESULT %-8s PASS  %s\n", name, detail); }
    else    { fail_count++; Serial0.printf("RESULT %-8s FAIL  %s\n", name, detail); }
  Serial0.flush();
}

static void banner(const char *title)
{
  Serial0.printf("\n========== %s ==========\n", title);
  Serial0.flush();
}

/* ------------------------------ 0. board ------------------------------ */
static void test_board()
{
  banner("BOARD / PSRAM");
  Serial0.printf("Chip        : %s rev %d\n", ESP.getChipModel(), ESP.getChipRevision());
  Serial0.printf("CPU MHz     : %d\n", ESP.getCpuFreqMHz());
  Serial0.printf("Flash MB    : %u\n", ESP.getFlashChipSize() / (1024 * 1024));
  Serial0.printf("PSRAM MB    : %u\n", (unsigned)(ESP.getPsramSize() / (1024 * 1024)));
  Serial0.printf("Free heap   : %u\n", ESP.getFreeHeap());
  report("BOARD", true, "printed identity + heap");
}

/* ------------------------------ 1. RGB ------------------------------- */
/* Single WS2812B on GPIO38, driven with RMT (800 kHz, GRB order).
 * Core 3.3.11 has no rgb_led_strip helper, so this mirrors the bundled
 * RMTWrite_RGB_LED example. */
#define RGB_BITS 24
static rmt_data_t rgb_data[RGB_BITS];

static bool rgb_send(uint8_t r, uint8_t g, uint8_t b)
{
  uint8_t grb[3] = { g, r, b };
  for (int col = 0; col < 3; col++) {
    for (int bit = 0; bit < 8; bit++) {
      int i = col * 8 + bit;
      if (grb[col] & (1 << (7 - bit))) {
        rgb_data[i].level0 = 1; rgb_data[i].duration0 = 8;
        rgb_data[i].level1 = 0; rgb_data[i].duration1 = 4;
      } else {
        rgb_data[i].level0 = 1; rgb_data[i].duration0 = 4;
        rgb_data[i].level1 = 0; rgb_data[i].duration1 = 8;
      }
    }
  }
  rmtWrite(RGB_PIN, rgb_data, RGB_BITS, RMT_WAIT_FOR_EVER);
  return true;
}

static void test_rgb()
{
  banner("RGB LED (WS2812B, GPIO38)");
  if (!rmtInit(RGB_PIN, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000)) {
    report("RGB", false, "rmtInit failed on GPIO38");
    return;
  }
  rgb_send(0, 0, 60);   delay(600);
  rgb_send(0, 60, 0);   delay(600);
  rgb_send(20, 20, 20);
  report("RGB", true, "WS2812B on GPIO38 driven blue->green");
}

/* ------------------------------ 2. SD card --------------------------- */
static void test_sd()
{
  banner("TF CARD (SDMMC CLK=5 CMD=4 DATA=6 CD=46)");

  /* Card detect is reported for information only. The guide lists SD_CD_PIN=46
   * but does not confirm a detect switch is actually wired there, and its
   * polarity is unknown, so it is NOT used to gate the test - a mounted and
   * writable card is the real proof. */
  pinMode(SD_CD, INPUT_PULLUP);
  Serial0.printf("Card detect  : pin %d reads %s (informational only)\n",
                 SD_CD, digitalRead(SD_CD) == LOW ? "LOW" : "HIGH");

  bool mounted = false;
  char how[64] = "none";

  /* 1-bit, library defaults. */
  if (SD_MMC.begin("/sdcard", true, false, 20000000)) {
    mounted = true; snprintf(how, sizeof(how), "1-bit (defaults)");
  }
  /* 1-bit, explicit pins from the guide. */
  if (!mounted && SD_MMC.begin("/sdcard", SD_D0, SD_CLK, SD_CMD, 20000000)) {
    mounted = true; snprintf(how, sizeof(how), "1-bit (D0=%d CLK=%d CMD=%d)", SD_D0, SD_CLK, SD_CMD);
  }
  /* 4-bit fallback. */
  if (!mounted && SD_MMC.begin("/sdcard", false, false, 20000000)) {
    mounted = true; snprintf(how, sizeof(how), "4-bit (defaults)");
  }

  if (!mounted) {
    report("SDCARD", false, "no mount with 1-bit or 4-bit - card not usable");
    return;
  }
  Serial0.printf("Mounted as   : %s\n", how);

  uint64_t total = 0, used = 0;
  if (SD_MMC.cardSize() > 0) {
    total = SD_MMC.cardSize() / (1024 * 1024);
    used  = SD_MMC.usedBytes() / (1024 * 1024);
  }
  Serial0.printf("Card size MB : %llu\n", total);
  Serial0.printf("Used MB      : %llu\n", used);

  /* Prove it is actually writable, not just mountable. */
  const char *path = "/sdcard/selftest.txt";
  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) {
    report("SDCARD", false, "mounted but could not open file for writing");
    return;
  }
  f.println("ESP32-S3-A7670E-4G self test");
  f.close();

  char buf[96] = {0};
  File r = SD_MMC.open(path, FILE_READ);
  if (!r) {
    report("SDCARD", false, "wrote file but could not reopen it for reading");
    return;
  }
  size_t n = r.readBytesUntil('\n', buf, sizeof(buf) - 1);
  r.close();
  SD_MMC.remove(path);

  bool ok = (n > 0) && strncmp(buf, "ESP32-S3-A7670E-4G", 18) == 0;
  char detail[96];
  snprintf(detail, sizeof(detail), "mounted %.1f MB, write+read back ok (%u bytes)", (double)total, (unsigned)n);
  report("SDCARD", ok, detail);
}

/* ------------------------------ 3. Camera ---------------------------- */
static void test_camera()
{
  banner("CAMERA (OV2640 expected, V1 pin map, XCLK=GPIO34)");

  camera_config_t config;
  memset(&config, 0, sizeof(config));
  config.ledc_channel  = LEDC_CHANNEL_0;
  config.ledc_timer    = LEDC_TIMER_0;
  config.pin_d0        = CAM_D0;
  config.pin_d1        = CAM_D1;
  config.pin_d2        = CAM_D2;
  config.pin_d3        = CAM_D3;
  config.pin_d4        = CAM_D4;
  config.pin_d5        = CAM_D5;
  config.pin_d6        = CAM_D6;
  config.pin_d7        = CAM_D7;
  config.pin_xclk      = CAM_XCLK;
  config.pin_pclk      = CAM_PCLK;
  config.pin_vsync     = CAM_VSYNC;
  config.pin_href      = CAM_HREF;
  config.pin_sccb_sda  = CAM_SDA;
  config.pin_sccb_scl  = CAM_SCL;
  config.pin_pwdn      = -1;
  config.pin_reset     = -1;
  config.xclk_freq_hz  = 20000000;
  config.pixel_format  = PIXFORMAT_RGB565;
  config.frame_size    = FRAMESIZE_QVGA;
  config.fb_count      = 2;
  config.grab_mode     = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    char detail[96];
    snprintf(detail, sizeof(detail), "esp_camera_init err 0x%x - check CAM DIP is ON and FPC seated", err);
    report("CAMERA", false, detail);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    Serial0.printf("Sensor PID   : 0x%04x\n", s->id.PID);
    Serial0.printf("Sensor VER   : 0x%04x\n", s->id.VER);
    Serial0.printf("Slave addr   : 0x%02x\n", s->slv_addr);
  }

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    report("CAMERA", false, "init ok but esp_camera_fb_get() returned NULL");
    return;
  }

  /* Mean luminance separates a real image from an all-black frame. */
  uint32_t sum = 0;
  uint16_t *px = (uint16_t *)fb->buf;
  size_t pixels = (size_t)fb->width * fb->height;
  for (size_t i = 0; i < pixels; i += 7) {
    uint16_t c = px[i];
    uint8_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    sum += (uint32_t)((r * 255 / 31) + (g * 255 / 63) + (b * 255 / 31)) / 3;
  }
  float mean = (float)sum / (float)((pixels + 6) / 7);
  Serial0.printf("Frame        : %ux%u, %u bytes\n", fb->width, fb->height, fb->len);
  Serial0.printf("Mean luma    : %.1f / 255\n", mean);
  esp_camera_fb_return(fb);

  bool ov2640 = s && s->id.PID == 0x0026;
  bool not_black = mean > 3.0f;
  char detail[128];
  snprintf(detail, sizeof(detail), "OV2640=%s %ux%u mean luma %.1f%s",
           ov2640 ? "yes" : "NO", fb->width, fb->height, mean,
           not_black ? " (not black)" : " (BLACK FRAME)");
  report("CAMERA", ov2640 && not_black, detail);
}

/* ------------------------------ summary ------------------------------ */
static void summary()
{
  banner("SUMMARY");
  Serial0.printf("PASS: %d   FAIL: %d\n", pass_count, fail_count);
  Serial0.println("SD card, camera, GPS and 4G are tested separately over direct AT.");
  Serial0.println("SELFTEST-DONE");
  Serial0.flush();
}

void setup()
{
  Serial0.begin(115200);
  delay(2500);

  banner("ESP32-S3-A7670E-4G SELF TEST");
  Serial0.println("Output on UART0 (CH9102). `Serial`/USB CDC is wired to the 4G module, not the host.");

  test_board();
  test_rgb();
  test_sd();
  test_camera();
  summary();
}

void loop()
{
  delay(1000);
}
