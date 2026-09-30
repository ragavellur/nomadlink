# NomadLink — Product Specification

- **Product:** NomadLink
- **Device:** NomadLink One (ESP32-S3 + SIMCom A7670E)
- **Firmware:** NomadLink OS
- **Web portal:** NomadLink Console
- **Cloud/telemetry:** NomadLink Cloud
- **Version:** 0.1.0
- **Derived from:** `esp32_s3_4g_router_product_requirement_document.md` v2.0

> **Read this first.** This specification is derived from the source PRD, but the
> PRD contains several factual errors about the actual NomadLink One hardware.
> Where a physical measurement disagrees with the PRD, the measurement wins.
> See [ADR-002](../decisions/ADR-002-hardware-facts-override-prd.md). The
> machine-readable requirements with their verification evidence are in
> [`requirements.json`](requirements.json).

## 1. Vision

NomadLink One is a travel gateway. A traveller connects their phone or laptop to
its Wi-Fi network and gets internet access, location, a camera, SMS and voice
telephony through a single cellular-backed device, with no dependence on a local
fixed line and no dependence on a cloud service for core function.

**Target users:** travellers, field technicians, and anyone who needs a
self-contained connectivity and tracking device.

**Core value:** one device, one power source, works anywhere with cell coverage,
and keeps working when the local Wi-Fi does not.

## 2. Scope

### In scope

Dual-WAN failover routing, camera streaming, GNSS location with cellular
fallback, direct MQTT telemetry with a remote command engine, SMS command
interface, telephony, an offline voice-activated dialling engine, and a web
management console.

### Out of scope for v0.1.0

- Cloud-hosted device management beyond the MQTT telemetry path
- Multi-device fleet management UI
- Native mobile apps
- Anything requiring a PC, bridge or relay to function (see ADR-004)

### Future scope

- OTA firmware updates
- Historical track playback and geofencing
- Additional cloud broker adapters beyond HiveMQ and Adafruit IO
- Adafruit IO parity (the PRD lists it; only HiveMQ is planned for v0.1.0)

## 3. Verified hardware baseline

This is the part of the product that is physically proven. It is the
foundation everything else is built on.

| Capability | Command / method | Verified result |
|---|---|---|
| GNSS position | `AT+CGNSSPWR=1`, `AT+CGNSSPORTSWITCH=0,1`, `AT+CGNSSTST=1` | `18.480816, 73.898640` LOCK t+130s (2026-09-30); `18.480776, 73.897998` in 69 s (2026-09-29); 17 GPS + 10 GLONASS |
| Cellular LBS fallback | `AT+CLBS=1,1` | `+CLBS: 0,18.481703,73.897415,550` |
| LTE registration | `AT+CEREG?` | `0,1` on Airtel, MCC 40490 MNC 90 |
| Data bearer | `AT+CGACT=1,1`, `AT+CGPADDR=1` | `+CGPADDR: 1,100.90.93.199` |
| Signal | `AT+CSQ` | `26,99` with the 4G patch antenna attached (`16,99` without) |
| Broker socket | `AT+CIPOPEN=0,"TCP","host",port` | `+CIPOPEN: 0,10` |
| Camera | OV2640 DVP | MID `0x7fa2`, PID `0x0026`, live 320×240, luma stddev 15.4 |
| SD card | `SD_MMC.setPins(5,4,6)` | 3.98 GB FAT32, write/read/delete pass (**Arduino only**, see RISK-006) |
| SMS out | `AT+CMGS` | `+CMGS: 2`, `+CMGS: 3`, both received |
| Voice out | `ATD<number>;` | `+VOICE CALL: BEGIN`, `+CLCC: ... 2` active |

### Critical operating preconditions

Two things must be true or capabilities silently fail. Both are easy to forget:

1. **The 4G patch antenna must be attached.** Without it `+CSQ` drops from 26 to
   16 and `AT+CLBS` returns `+CLBS: 9`.
2. **`AT+SIMEI` must be populated** from `AT+CGSN` before LBS will work.
   Unpopulated gives `+CLBS: 12`.

And one thing must never be done: **do not power-cycle the GNSS engine.** It
needs one uninterrupted power-on to download a fresh almanac. The module will
answer `AT` and emit checksum-valid NMEA even when it was never properly started,
which mimics "no sky view."

## 4. Corrections to the source PRD

| PRD claim | Correction | Evidence |
|---|---|---|
| FR-3.1: `AT+CGNSINF` | Does not exist. `AT+CGPSINFO` is valid. | `+CME ERROR` on `AT+CGNSINF` |
| FR-1.3: PPP Mode | PPP has never produced an IP on this device. The working path is the modem's own TCP/IP stack via `AT+CIPOPEN`. | nomad-sentinel 1.5: `+CGREG: 0,3`, `CGACT 1,0` |
| Hardware table: I2S mic + DAC | Presence and wiring unconfirmed on this board | nomad-sentinel 1.11 UNSUPPORTED (RISK-003) |
| NFR 7.1: 10–15 Mbps NAT | Cat-1 bis modem. Unmeasured, likely unachievable alongside video and DSP | RISK-001 |
| NFR 7.2: <800 ms speech | Never benchmarked | RISK-004 |
| NFR 4.4: TLS 1.2/1.3 | Untested on this firmware; only plaintext 1883 proven | RISK-005 |
| (omitted) | **Cellular LBS fallback is a real working capability and is now a requirement** | `+CLBS: 0,18.481703,73.897415,550` |

## 5. Known blockers

Three of these determine whether major requirements are achievable at all. They
are Sprint 1.

| Risk | Question | Consequence if the answer is no |
|---|---|---|
| RISK-002 | Does PPP produce an IP on the Airtel SIM? | REQ-004 NAT routing is not achievable; descope or change modem |
| RISK-003 | Are the I2S mic and DAC populated? | REQ-017 full-duplex and REQ-018/019 ESP-SR descope entirely |
| RISK-001 | What NAT throughput is actually achievable? | PRD NFR must be renegotiated against a measured figure |

## 6. Requirements

23 requirements across 9 epics and 23 features, in
[`requirements.json`](requirements.json). Summary:

| Status | Count |
|---|---|
| VERIFIED | 2 |
| PARTIALLY_VERIFIED | 6 |
| NOT_STARTED | 15 |

The only fully VERIFIED requirements are **REQ-007** (GNSS fix) and **REQ-008**
(cellular LBS fallback) — both physically measured. Six are partially verified,
where a lower layer works but the product feature does not. Counts are generated
by `render.py`; do not hand-edit.

Traceability, evidence and per-requirement verification notes live in the
generated dashboard at [`../project/requirements.html`](../project/requirements.html).

## 7. Not yet proven

Stated plainly so nobody assumes otherwise:

- **End-to-end MQTT publish from the device to a broker.** The socket opens, but
  the MQTT CONNECT has never been confirmed. See BUG-002 and TEST-403 (FAIL).
- **NAT routing.** Never attempted; blocked on RISK-002.
- **Any web portal functionality.** Not started.
- **Audio hardware.** Unconfirmed.
- **TLS on the cellular path.** Unconfirmed.
