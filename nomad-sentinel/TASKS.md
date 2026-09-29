# Nomad Sentinel Blackbox — Task Tracker

Status legend: `[ ]` pending · `[~]` in progress · `[x]` done · `[!]` blocked/needs user

---

## Milestone 1 — Bring-up & hardware audit (prove every assumption)

- [x] 1.1 Install/verify ESP-IDF toolchain on Mac  (v6.1-dev, ~/esp/esp-idf, idf.py OK)
- [x] 1.2 Scaffold `nomad-sentinel` IDF project + partitions.csv + sdkconfig.defaults (builds+runs)
- [x] 1.3 Probe PSRAM usable size + correct mode (QSPI/OPI), flash size  (2048KB usable, QUAD; free_heap 2.46MB / internal 394KB)
- [x] 1.4 Probe modem AT on GPIO17(RX)/18(TX) @115200  — UART link OK @115200. A7670E-FASE, FW A7670M7_V1.11.1, IMEI <IMEI redacted>. SIM: +CPIN READY, +CGREG 0,11, +CSQ 17,99, +CGNSSPWR 1 OK. (DONE 13 Sep)
- [~] 1.5 Probe data path: USB-PPP via esp_modem usb_host_cdc_acm (descriptor, dial, ping) / UART-PPP fallback
  — **USB-PPP code path COMPLETE (14 Sep):** DIPs corrected per official docs: **4G OFF** (enables GPIO33 software modem power control) + **USB OFF** (routes modem USB to ESP32) + **HUB ON** (keeps console/flash ports alive). Auto-recovery via GPIO33 power-cycle eliminates physical unplug/replug after every flash-reset: modem re-attaches in ~7s automatically (esp_modem_new_dev_usb_safe + exception-safe C++ wrapper). Enumeration: VID 0x1E0E PID 0x9011, 6 interfaces (AT=4, data=5). AT diagnostics via esp_modem_at: CPIN READY, CFUN 1, CSQ 10-18, COPS "40490"/LTE. ATD*99# with AT+CGDCONT=1,"IP","jionet": CONNECT → PPP Start/Establish/Auth/Network → LCP/IPCP negotiated → then +PPPD: DISCONNECT after ~30s with no IP. **Root cause: AT+CGREG? returns 0,3 (PS registration denied by Jio network) → CGACT 1,0 (PDP context not activated).** This is a SIM/provisioning/coverage issue, not firmware. USB-PPP path is [x] done — need active Jio SIM with data plan + outdoor antenna to confirm CGREG: 0,1 and IP assignment.
- [ ] 1.6 Probe GNSS via AT+CGPSINFO (needs antenna + sky view)
- [!] 1.7 Probe SD mount + write bandwidth (SDMMC 1-bit: CLK5/CMD4/DATA6)  — BLOCKED: hardware. Verified GPIO5=CLK/4=CMD/6=D0/46=CD from official A7670E schematic netlist. 64GB+4GB cards work on PC but get ZERO response on board (CMD0 no-response OK; CMD8/CMD55/ACMD41/CMD5/CMD1 all err=0x107). Host 400kHz 1-bit internal pullup. Card socket on this board suspect (no VDD/contact). Deferred until a replacement board/card-slot is available.
- [x] 1.8 Probe camera roll in IDF (esp32-camera 2.1.7) — V1 map OK, streaming QVGA JPEG ~4.4KB, CRC changes
- [ ] 1.9 Hunt USB-power (VBUS) detect GPIO for CAR/TREK
- [ ] 1.10 Probe BOOT button (GPIO0) usable as UX button
- [ ] 1.11 Verify audio path: does any mic/codec reach the ESP32? (expect UNSUPPORTED)
- [ ] 1.12 Write `docs/HARDWARE.md` with only verified facts

## Milestone 2 — Storage, GNSS, Button foundation

- [ ] 2.1 `StorageManager`: canonical layout, dirs, atomic config writes, flush/close, free-space
- [ ] 2.2 `SystemLogger`: events.log / boot.log / crash.log, event taxonomy
- [ ] 2.3 `GNSSManager`: 1 Hz fix parse, fix/lost events
- [ ] 2.4 `TrailLogger`: /trek/YYYY-MM-DD/track.csv (5 s default)
- [ ] 2.5 `ButtonManager`: debounce, tap(<1 s), hold 2 s, hold 5 s, cancel 3 s, non-blocking

## Milestone 3 — Modes & power

- [ ] 3.1 `ModeManager`: CAR/TREK decision via USB detect + hysteresis
- [ ] 3.2 CAR→TREK and TREK→CAR transition (flush, periph power)
- [ ] 3.3 `PowerManager`: battery states (MAX17048 NORMAL/LOW/CRITICAL), CRITICAL shutdown path

## Milestone 4 — Modem services (calls / SMS / GNSS)

- [ ] 4.1 `ModemManager`: init, CPIN, CREG/CEREG, CSQ, URC handling, recovery
- [ ] 4.2 SMS send/recv
- [ ] 4.3 `CallManager`: ATD/CLCC/CHUP state machine, GPS trail keeps running
- [ ] 4.4 `ContactManager`: contacts.json CRUD (validated, atomic), lookup by normalized name
- [ ] 4.5 `VoiceManager`: marks offline ESP-SR **UNSUPPORTED** unless 1.11 finds a path

## Milestone 5 — Cellular NAT router

- [ ] 5.1 Integrate/adapt `martin-ger/esp32_nat_router` (lwIP NAT, IP fwd, DHCP, portmap, web, CLI)
- [ ] 5.2 `CellularUplink`: USB-PPP netif as uplink (fallback UART-PPP), up/down event
- [ ] 5.3 WiFi AP (NOMAD-SENTINEL default) + DHCP, 192.168.4.1
- [ ] 5.4 Connection monitor + reconnect without reboot
- [ ] 5.5 Port forwarding via router maps

## Milestone 6 — Dashcam (CAR mode)

- [ ] 6.1 Continuous segmented MJPEG-AVI recording (60 s default, configurable)
- [ ] 6.2 Telemetry CSV per segment
- [ ] 6.3 Retention: auto-delete oldest dashcam only; storage priority

## Milestone 7 — SOS

- [ ] 7.1 Hold-5 s countdown (3/2/1), release cancels
- [ ] 7.2 SOS SMS (location + maps link + time + battery + fix)
- [ ] 7.3 Pending queue + periodic retry without coverage
- [ ] 7.4 Cancel (hold 3 s) + optional cancel SMS

## Milestone 8 — Web UI & first boot

- [ ] 8.1 Pages: / /status /setup /wifi /cellular /contacts /sos /router /system
- [ ] 8.2 First-boot setup wizard (NOMAD-SENTINEL @ 192.168.4.1)
- [ ] 8.3 System page: events, versions, reboot; (optional) OTA

## Milestone 9 — Acceptance & docs

- [ ] 9.1 Acceptance tests 1–10 (spec §52) on hardware
- [ ] 9.2 README.md
- [ ] 9.3 docs/ARCHITECTURE.md, NAT_ROUTER.md, CELLULAR.md, VOICE_CALLING.md, CONFIGURATION.md, STORAGE.md, TESTING.md

---

## User action queue (what I need from you)
- [!] 4G SIM: CGREG returns 0,3 (PS registration denied by network) — verify Jio SIM has active data plan + try a different SIM if available
- [ ] microSD (FAT32)
- [ ] 18650 battery
- [ ] 4G antenna + GNSS antenna (IPEX1) — outdoor test to confirm registration + GPS fix
- [ ] Current DIPs: CAM ON, **4G OFF**, USB OFF, HUB ON (4G OFF enables GPIO33 modem power control — do NOT flip to 4G ON or the unplug dance returns)
- [ ] Place board near window/outdoors for GPS/data tests