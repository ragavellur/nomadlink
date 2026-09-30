# NAT Router base — vendored upstream

This directory is the **unmodified upstream source** of `esp32_nat_router`, kept here
as the base for NomadLink One firmware. It is vendored verbatim on purpose: NomadLink
adds features on top of a known-good router rather than reimplementing NAT, DHCP or
the web console.

| Field | Value |
|---|---|
| Upstream | https://github.com/martin-ger/esp32_nat_router |
| Version | 2.4.17 (`CMakeLists.txt` `PROJECT_VER`) |
| Commit | `2fe7a4c` — "Firmware binaries 2.4.17 for all seven targets" |
| Upstream licence | **Not declared.** No `LICENSE` file upstream; GitHub reports `license: null`. Only the bundled WireGuard submodule carries an explicit BSD-style licence. See RISK entry. |

## What is vendored here

`main/`, `components/`, `include/`, `CMakeLists.txt`, `sdkconfig.defaults`,
`sdkconfig.defaults.esp32s3`, `partitions_example.csv`.

`main/component.mk` (PlatformIO-only) was dropped; this project builds with
`idf.py`. Upstream also ships PlatformIO support — `platformio.ini` is not
vendored, so PlatformIO builds are not supported here.

## Target

ESP32-S3. The router design is native lwIP forwarding:

- `CONFIG_LWIP_IP_FORWARD=y`
- `CONFIG_LWIP_IPV4_NAPT=y` — `ip_napt_enable(my_ap_ip, 1)`
- `CONFIG_LWIP_L2_TO_L3_COPY=y` — required by the packet hooks; it is *not* a
  NAT switch
- AP DHCP server via the bundled `dhcpserver` component

## Build

Requires **ESP-IDF 5.5.x**. It does not build on 6.1-dev: `main` requires the
component `json`, which moved into the component manager in IDF 6.x
(`Failed to resolve component 'json' required by component 'main': unknown name`).
An IDF 5.5.4 install is required — do not "fix" this by editing the component list,
that diverges from the base.

```bash
. ~/esp/esp-idf-5.5.4/export.sh
idf.py -B build_esp32s3 -DIDF_TARGET=esp32s3 set-target esp32s3
idf.py -B build_esp32s3 build
```

There is **no local compile gate for this tree yet** (`make product` still builds
the old Arduino sketch). Do not treat this directory as gated until
`scripts/project-tracker/build-product.sh` points here.

## Binaries served by the web installer

`docs/project/firmware/*.bin` are upstream's own prebuilt ESP32-S3 artifacts,
byte-identical to `firmware_esp32s3/` upstream, already confirmed working on this
board. They are served as-is; NomadLink features are **not** in them yet.

## NomadLink divergences from upstream (identity only)

Applied so the product and its artifact carry the NomadLink name. No behavioural change:

| Upstream | NomadLink | Where |
|---|---|---|
| project `esp32_nat_router` → `esp32_nat_router.bin` | project `nomadlink` → **`nomadlink.bin`** | `CMakeLists.txt` |
| AP SSID `ESP32_NAT_Router` | **`NomadLink`** | `main/esp32_nat_router.c` |
| hostname `esp32-nat-router` | **`nomadlink`** | `include/router_config.h` |
| UI titles, console banner, hostname placeholder | `NomadLink` | `components/http_server/pages/page_index.h`, `components/remote_console/` |
| MQTT topic prefix `esp32_nat_router` | **unchanged** | functional broker identifier; renaming breaks subscriptions |

The IDF project name is what `idf.py` names the output binary after, so renaming the
project is the only way to rename `nomadlink.bin`.

Rebrand, then restage — or the installer keeps flashing the old image:

```bash
make stage-firmware
```

`make product` warns when the staged payload is older than the sources it came from.

## NomadLink changes (to be added here, not alongside)

- A7670E 4G PPP netif as an additional uplink, over UART GPIO17/18
  (the Espressif `usb_cdc_4g_module` example uses USB CDC; transport must change,
  its PPP/NAPT approach must not)
- GNSS, camera, SD, SMS, MQTT

## Board notes

- A7670E UART: RX 18 / TX 17 (`ss.begin(115200, SERIAL_8N1, 17, 18)`)
- module-enable rail GPIO33; **PWRKEY is not wired to this board** — never pulse it
- negotiated modem baud is `921600`