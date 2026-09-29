# ESP32-S3-A7670E-4G Camera Guide

How to use the OV2640 camera on the **Waveshare ESP32-S3-A7670E-4G** board, the problems
encountered, and the solutions — for later reference.

---

## 1. Working Configuration (verified)

The board is the Waveshare **V1** layout (not V2). This exact pin map produces frames:

| Signal | GPIO | Signal | GPIO |
|---|---|---|---|
| XCLK | 34 | HREF | 35 |
| PCLK | 37 | VSYNC | 36 |
| SIOD (SDA) | 15 | D0..D7 (Y2..Y9) | 7..14 |
| SIOC (SCL) | 16 | PWDN / RESET | -1 / -1 |

Config example:

```cpp
camera_config_t config;
memset(&config, 0, sizeof(config));
config.ledc_channel   = LEDC_CHANNEL_0;
config.ledc_timer     = LEDC_TIMER_0;
config.pin_d0         = 7;    // Y2
config.pin_d1         = 8;    // Y3
config.pin_d2         = 9;    // Y4
config.pin_d3         = 10;   // Y5
config.pin_d4         = 11;   // Y6
config.pin_d5         = 12;   // Y7
config.pin_d6         = 13;   // Y8
config.pin_d7         = 14;   // Y9
config.pin_xclk       = 34;
config.pin_pclk       = 37;
config.pin_vsync      = 36;
config.pin_href       = 35;
config.pin_sccb_sda   = 15;
config.pin_sccb_scl   = 16;
config.pin_pwdn       = -1;
config.pin_reset      = -1;
config.xclk_freq_hz   = 20000000;
config.pixel_format   = PIXFORMAT_JPEG;
config.frame_size     = FRAMESIZE_QVGA;   // or VGA
config.jpeg_quality   = 12;               // lower = better quality
config.fb_count       = 2;
config.grab_mode      = CAMERA_GRAB_WHEN_EMPTY;
esp_err_t err = esp_camera_init(&config);
```

Expected identity on success: `PID=0x0026 VER=0x42 addr=0x30` (OV2640). Grab with
`esp_camera_fb_get()`, dump `fb->buf` / `fb->len` (JPEG), then `esp_camera_fb_return(fb)`.

### V2 layout (for reference, other Waveshare boards)

| Signal | GPIO |
|---|---|
| XCLK | 39 |
| PCLK | 46 |
| VSYNC | 42 |
| HREF | 41 |
| SIOD / SIOC | 15 / 16 |
| D0..D7 | 7..14 |

---

## 2. Environment

- **Host**: macOS. Board serial tools:
  - `/dev/cu.usbmodem58750034621` — **CH9102, ESP32 UART console**. All firmware `Serial0` output appears here. Capture from this port.
  - `/dev/cu.wchusbserial58750034621` — **CH343, flashing / reset** (esptool). Sometimes "Resource busy" — just retry.
- **Toolchain**:
  ```bash
  export ARDUINO_DATA_DIR=/Users/raghavan/Library/Arduino15
  export ARDUINO_SKETCHBOOK_DIR=/Users/raghavan/Documents/Arduino
  ARC="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
  ```
- **FQBN** (this one boots; see PSRAM note below):
  ```
  esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=dio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,PSRAM=enabled,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default
  ```

---

## 3. Build, flash, capture

```bash
# build
$ARC compile --fqbn "$FQBN" --build-path $PWD/build ./cap_frame

# flash app only (bootloader/partitions already correct on chip)
python3 -m esptool --chip esp32s3 -p /dev/cu.wchusbserial58750034621 \
  -b 921600 write_flash 0x10000 ./build/cap_frame.ino.bin
```

Working sketch: `cap_frame/cap_frame.ino` — initializes the camera with the V1 map,
prints `GOTFRAME <n>` + raw JPEG bytes + `ENDFRAME` over the UART. Capture and decode:

```python
import serial, time
s = serial.Serial('/dev/cu.usbmodem58750034621', 115200, timeout=0.5)
buf = b''
while b'ENDFRAME' not in buf:
    buf += s.read(65536)
i = buf.index(b'GOTFRAME'); e = buf.index(b'\n', i)
n = int(buf[i:e].split()[1])
jpg = buf[e+1:e+1+n]
open('frame.jpg','wb').write(jpg)          # verify: PIL Image.open()
```

Verified outputs: `photos/frame0.jpg` (QVGA 320x240), `photos/vga_frame.jpg` (VGA 640x480) — both decode as valid RGB JPEGs.

---

## 4. Problems faced and solutions

1. **"Detected camera not supported." (error 0x106)** — caused by the camera not being
   powered or a wrong board config. Fixes, applied in order:
   - Flip the **CAM DIP** switch (back of board) to ON and reseat the FPC.
   - Use a working board FQBN (PSRAM enabled, see below).
   - **Use the correct XCLK pin: GPIO34 for V1.** With XCLK=39 the probe fails;
     XCLK=34 detects the OV2640. (XCLK is generated on the given GPIO via LEDC.)

2. **PSRAM boot loop (`RTCWDT_RTC_RST`)** — `PSRAM=opi` loops forever. Must use
   `PSRAM=enabled` (QSPI mode). Board reports 2 MB PSRAM this way. Symmetric problem:
   `PSRAM=disabled` boots but the camera fails with
   `cam_hal: cam_dma_config(509): frame buffer malloc failed` because the frame
   buffers can't be allocated without PSRAM.

3. **`esp_camera_fb_get()` always NULL / no frames** — the long blocker. Root cause:
   - The pin map being tested was the wrong version of the Waveshare layout, and the
     D0..D7 assignments were reversed (13..6 instead of 7..14). The correct, **documented**
     V1 map (table above) with the right data order streams frames immediately.
   - Testing alternate maps by `esp_camera_deinit()` + re-init **wedges the driver**.
     Always reboot cleanly into the target map instead of toggling at runtime.
   - Data pin order does not matter for detection or fb_get timing, but it must be
     correct for sane pixel colors — keep Y2..Y9 = GPIO 7..14.

4. **Console silent / output going nowhere** — on this board `Serial` (USB CDC) is
   wired to the **4G modem's USB**, not the host. Print to `Serial0` (UART0 → CH9102 →
   `/dev/cu.usbmodem...`). This wasted one capture attempt before the fix.

5. **GPIO debugging pitfalls (avoid)**
   - GPIO 22–25 are invalid on ESP32-S3 (`gpio_set_direction: GPIO number error`).
   - GPIO 30–32 are wired to the 4G/modem section; touching them resets the board.
   - Sustained tight GPIO polling with the camera running trips the watchdog
     (`TG1WDT_SYS_RST`). Do not brute-force GPIO scans; measure with short bursts only.

6. **Detection can be timing/flaky** — camera SCCB probe succeeds even without XCLK
   in some cases and fails others; one probe result alone is not authoritative. If the
   sensor is confirmed on the SCCB bus (GPIO 15/16 have external pull-ups), boot a clean
   sketch with the correct map rather than chasing detection.

---

## 5. Quick reference

- Camera identity: OV2640, PID 0x0026, VER 0x42, SCCB addr 0x30 on GPIO 15/16.
- Camera driver API headers (ESP32 core 3.3.11 bundled):
  `.../esp32s3-libs/3.3.11/include/espressif__esp32-camera/driver/include/esp_camera.h`
  and `sensor.h` (`sensor_t` fields: `id.PID`, `id.VER`, `id.MIDH/MIDL`, `slv_addr`;
  grab enums `CAMERA_GRAB_WHEN_EMPTY` / `CAMERA_GRAB_LATEST`).
- Official Waveshare pin tables:
  https://docs.waveshare.com/ESP32-S3-A7670E-4G/Arduino
- Workspace sketches: `waveshare_pins/` (tests V1/V2/V1-swap and reports frames),
  `cap_frame/` (streams JPEG over UART), plus earlier debuggers (`camera_xclk`,
  `psram_test`, `pin_hunter`, `syncsweep`, ...).