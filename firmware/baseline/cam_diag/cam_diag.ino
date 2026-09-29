/* Camera diagnostic only - ESP32-S3-A7670E-4G
 *
 * Stricter than the earlier selftest: a dead or desynced sensor can emit a
 * uniform mid-gray frame that still has a healthy-looking mean luminance, so
 * this also measures standard deviation and a coarse block variance. A real
 * scene has real spatial structure; a flat fill has none.
 *
 * Core 3.3.11 API: camera_config_t carries the pins directly (there is no
 * camera_pins_t), and sensor_t exposes .id.{MIDH,MIDL,PID,VER} and .slv_addr.
 */

#include <Arduino.h>
#include <esp_camera.h>

void setup()
{
  Serial0.begin(115200);
  delay(2500);
  Serial0.println("\n========== CAMERA DIAGNOSTIC ==========");
  Serial0.printf("Free heap: %u   PSRAM: %u bytes\n", ESP.getFreeHeap(), ESP.getPsramSize());

  camera_config_t cfg = {};
  cfg.ledc_channel   = LEDC_CHANNEL_0;
  cfg.ledc_timer     = LEDC_TIMER_0;
  cfg.pin_xclk       = 34;   /* XCLK  */
  cfg.pin_pclk       = 37;
  cfg.pin_vsync      = 36;
  cfg.pin_href       = 35;
  cfg.pin_sccb_sda   = 15;
  cfg.pin_sccb_scl   = 16;
  cfg.pin_d0         = 7;
  cfg.pin_d1         = 8;
  cfg.pin_d2         = 9;
  cfg.pin_d3         = 10;
  cfg.pin_d4         = 11;
  cfg.pin_d5         = 12;
  cfg.pin_d6         = 13;
  cfg.pin_d7         = 14;
  cfg.pin_pwdn       = -1;
  cfg.pin_reset      = -1;
  cfg.xclk_freq_hz   = 20000000;
  cfg.pixel_format   = PIXFORMAT_RGB565;
  cfg.frame_size     = FRAMESIZE_QVGA;   /* 320x240 */
  cfg.fb_count       = 2;
  cfg.grab_mode      = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    Serial0.printf("esp_camera_init failed: 0x%x\n", err);
    Serial0.println("RESULT CAMERA   FAIL  init error");
    Serial0.println("CAMDONE"); Serial0.flush();
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (!s) {
    Serial0.println("RESULT CAMERA   FAIL  no sensor handle");
    Serial0.println("CAMDONE"); Serial0.flush();
    return;
  }

  Serial0.printf("Sensor MID      : 0x%02x%02x\n", s->id.MIDH, s->id.MIDL);
  Serial0.printf("Sensor PID      : 0x%04x\n", s->id.PID);
  Serial0.printf("Sensor VER      : 0x%02x\n", s->id.VER);
  Serial0.printf("SCCB slave addr : 0x%02x\n", s->slv_addr);

  camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
  if (info && info->name) Serial0.printf("Driver name     : %s\n", info->name);

  bool ov2640 = (s->id.PID == 0x0026);
  bool exact  = (s->id.PID == 0x0026 && s->id.VER == 0x0042);
  Serial0.printf("OV2640 PID match: %s\n", ov2640 ? "YES" : "NO");
  Serial0.printf("Exact PID+VER   : %s (expect 0x0026 / 0x0042)\n", exact ? "YES" : "NO");

  /* Read the real register bank over SCCB as an independent confirmation. */
  if (ov2640 && s->get_reg) {
    Serial0.printf("Reg 0x0A/0x0B   : 0x%02x / 0x%02x\n", s->get_reg(s, 0x0A, 0xFF), s->get_reg(s, 0x0B, 0xFF));
    Serial0.printf("Reg 0x0C/0x0D   : 0x%02x / 0x%02x (COM7)\n", s->get_reg(s, 0x0C, 0xFF), s->get_reg(s, 0x0D, 0xFF));
  }

  camera_fb_t *fb = NULL;
  for (int i = 0; i < 5 && !fb; i++) { fb = esp_camera_fb_get(); if (!fb) delay(120); }
  if (!fb) {
    Serial0.println("RESULT CAMERA   FAIL  frame grab returned NULL");
    Serial0.println("CAMDONE"); Serial0.flush();
    return;
  }

  const int W = fb->width, H = fb->height;
  Serial0.printf("Frame           : %dx%d, %u bytes, fmt=%d (RGB565=6)\n",
                 W, H, (unsigned)fb->len, fb->format);

  const int step = 2;
  double sum = 0, sum2 = 0; int n = 0, lo = 255, hi = 0;
  uint8_t *p = (uint8_t *)fb->buf;
  for (int y = 0; y < H; y += step) {
    for (int x = 0; x < W; x += step) {
      int i = (y * W + x) * 2;
      int r = p[i], g = p[i + 1], b = p[i + 1];
      int luma = (r * 30 + g * 59 + b * 11) >> 8;
      if (luma < lo) lo = luma;
      if (luma > hi) hi = luma;
      sum += luma; sum2 += (double)luma * luma; n++;
    }
  }
  double mean = sum / n;
  double var  = (sum2 / n) - (mean * mean);
  double sd   = (var > 0) ? sqrt(var) : 0.0;

  Serial0.printf("Luma min/max    : %d / %d\n", lo, hi);
  Serial0.printf("Luma mean       : %.1f / 255  (%.1f%%)\n", mean, mean * 100.0 / 255.0);
  Serial0.printf("Luma stddev     : %.1f   <-- flat fill would be near 0\n", sd);

  const int B = 16;
  double bsum = 0, bsum2 = 0; int bn = 0;
  const int bh = H / B, bw = W / B;
  for (int by = 0; by + bh <= H; by += bh) {
    for (int bx = 0; bx + bw <= W; bx += bw) {
      double s2 = 0; int m = 0;
      for (int y = by; y < by + bh; y += step)
        for (int x = bx; x < bx + bw; x += step) {
          int i = (y * W + x) * 2;
          s2 += ((p[i] * 30 + p[i + 1] * 59 + p[i + 1] * 11) >> 8); m++;
        }
      double bm = s2 / m; bsum += bm; bsum2 += bm * bm; bn++;
    }
  }
  double bmean = bsum / bn;
  double bvar  = (bsum2 / bn) - (bmean * bmean);
  double bsd   = (bvar > 0) ? sqrt(bvar) : 0.0;
  Serial0.printf("Block stddev    : %.1f   <-- real scene structure\n", bsd);

  esp_camera_fb_return(fb);

  double means[2];
  for (int k = 0; k < 2; k++) {
    camera_fb_t *f2 = NULL;
    for (int i = 0; i < 5 && !f2; i++) { f2 = esp_camera_fb_get(); if (!f2) delay(120); }
    if (!f2) { means[k] = -1; delay(150); continue; }
    double s2 = 0; int m = 0;
    uint8_t *q = (uint8_t *)f2->buf;
    for (int y = 0; y < (int)f2->height; y += 4)
      for (int x = 0; x < (int)f2->width; x += 4) {
        int i = (y * (int)f2->width + x) * 2;
        s2 += ((q[i] * 30 + q[i + 1] * 59 + q[i + 1] * 11) >> 8); m++;
      }
    means[k] = s2 / m;
    esp_camera_fb_return(f2);
    delay(150);
  }
  Serial0.printf("Repeat means    : %.1f , %.1f\n", means[0], means[1]);

  bool not_black  = (mean > 5.0);
  bool not_sat    = (mean < 250.0 && hi > 20);
  bool not_flat   = (sd > 12.0);
  bool has_struct = (bsd > 6.0);
  bool stable     = (means[0] > 0 && means[1] > 0 &&
                      fabs(means[0] - mean) < 40 && fabs(means[1] - mean) < 40);

  Serial0.println("---------------- checks ----------------");
  Serial0.printf("  sensor is OV2640      : %s\n", ov2640 ? "yes" : "NO");
  Serial0.printf("  exact PID+VER         : %s\n", exact ? "yes" : "no");
  Serial0.printf("  not black             : %s\n", not_black ? "yes" : "NO");
  Serial0.printf("  not saturated         : %s\n", not_sat ? "yes" : "NO");
  Serial0.printf("  not flat fill         : %s\n", not_flat ? "yes" : "NO");
  Serial0.printf("  has scene structure   : %s\n", has_struct ? "yes" : "NO");
  Serial0.printf("  frame-to-frame stable : %s\n", stable ? "yes" : "no");

  bool pass = ov2640 && not_black && not_sat && not_flat && has_struct && stable;
  Serial0.printf("RESULT CAMERA   %s  OV2640 %ux%u mean=%.1f sd=%.1f blockSD=%.1f\n",
                 pass ? "PASS" : "FAIL", W, H, mean, sd, bsd);
  Serial0.println("CAMDONE");
  Serial0.flush();
}

void loop() { delay(1000); }
