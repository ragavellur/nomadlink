# ADR-005 — Firmware framework selection

- **Status:** PROPOSED — blocks TASK-101, TASK-103, TASK-104
- **Date:** 2026-09-29
- **Related:** TASK-019, RISK-002, RISK-006, RISK-007

## Context

The PRD allows either **ESP-IDF 5.x** or **Arduino-ESP32 3.x** (NFR 8.1). This
choice has not been made and it blocks the entire network epic, because the NAT
router, the web server, the camera driver and the MQTT client are all chosen
differently on each.

The evidence currently on the bench is split, and honestly so:

**Arduino-ESP32 3.3.11 (all working hardware baselines):**

| Capability | Status |
|---|---|
| GNSS fix (69 s cold start) | Works |
| Cellular LBS fallback | Works |
| 4G data bearer + registration | Works |
| Camera OV2640 live capture | Works |
| SD card write/read/delete | Works |
| Outgoing voice call | Works |
| Outgoing SMS | Works |
| `AT+CIPOPEN` socket to broker | Opens |

**ESP-IDF (nomad-sentinel project):**

| Capability | Status |
|---|---|
| AT over UART | Works |
| Modem attach, CSQ, COPS | Works |
| Camera QVGA JPEG streaming | Works |
| PPP + IP assignment | **Fails** — `+CGREG: 0,3`, `CGACT 1,0`, no IP |
| SD card | **Fails** — `err=0x107` on all commands |
| NAT router | Never attempted |
| Audio path | Recorded UNSUPPORTED / unknown |

So the working evidence is on the Arduino side, but the *hardest* requirements
are ESP-IDF-native:

- **NAT routing (REQ-004)** — `esp32_nat_router` is an ESP-IDF component, which
  the PRD explicitly points to as the reference implementation for the web
  interface and NAT behaviour.
- **ESP-SR (REQ-018)** — an ESP-IDF component manager package.
- **`esp-mqtt` with TLS (REQ-013, REQ-022)** — ESP-IDF.
- **I2S / `esp_codec_dev` (REQ-017)** — ESP-IDF.

Arduino can consume some of these through the component manager or ports, but at
real integration cost, and NAT in particular is not a comfortable Arduino-ESP32
story.

## Decision

**PENDING.** This ADR is deliberately undecided. The options are:

**Option A — ESP-IDF 5.x as primary.**
Pros: NAT via `esp32_nat_router`, ESP-SR, `esp-mqtt` TLS, I2S and camera all
native. Matches the reference implementation the PRD names. The nomad-sentinel
project is already scaffolded. Cons: the six working Arduino baselines must be
ported, and two of them (SD, PPP) already *fail* under IDF for unknown reasons
(RISK-002, RISK-006), so porting is not risk-free.

**Option B — Arduino-ESP32 3.x as primary.**
Pros: all working baselines already run here. Lowest immediate risk. Cons: NAT
and ESP-SR are significantly harder; the highest-risk requirement in the product
lands on the weakest platform support.

**Option C — ESP-IDF primary, with the verified AT-command baselines retained as
a separate `firmware/baseline/` Arduino test harness for hardware regression.**
Pros: gets the best of both — hardware regression tests stay on the platform
where the hardware is already known to behave, while product code lands on the
platform that can actually deliver NAT and ESP-SR. Cons: two build systems to
maintain; a discipline burden to keep them in sync.

## Recommendation

**Option C**, conditional on two experiments being run first:

1. **RISK-002 / PPP on the current Airtel SIM.** If PPP cannot produce an IP even
   with a SIM that is proven to attach, then REQ-004 (NAT router) is not
   achievable on this hardware at all, and the framework question becomes moot
   because the headline feature must be descoped. The earlier PPP failure was on
   a **Jio** SIM with `+CGREG: 0,3` (registration denied), which is a
   provisioning symptom, not proof that PPP is broken here.
2. **RISK-003 / audio hardware population.** If there is no I2S mic and DAC on
   this board, REQ-017 and REQ-019 descope entirely and ESP-SR stops being a
   framework-selection driver.

Both experiments are cheap and both are blocking. Neither should be deferred
behind framework work.

## Consequences of not deciding

TASK-101, TASK-103 and TASK-104 remain `BACKLOG` and cannot move to `READY`,
because writing NAT code against a framework that may change is wasted motion.
The location epic (TASK-301, TASK-303) and the MQTT transport (TASK-405) are
deliberately kept framework-agnostic where possible so they are not blocked.

## Consequences of deciding Option A or C

- The `nomad-sentinel` project becomes the product firmware, and its
  `MODEM_USE_UART 0` / USB-PPP-first history must be rewritten to prefer the UART
  route that is proven to work.
- The Arduino sketches move to `firmware/baseline/` as regression harnesses.
- The Arduino core 3.3.11 FQBN and the ESP-IDF toolchain must both be
  documented and CI-supported.
