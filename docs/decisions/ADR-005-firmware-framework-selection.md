# ADR-005 — Firmware framework selection

- **Status:** ACCEPTED — Option B, Arduino-ESP32 3.x as the product framework (see *Update 2026-09-29 (later)*)
- **Date:** 2026-09-29
- **Related:** TASK-019, RISK-002, RISK-003, RISK-006, RISK-007

## Superseded 2026-10-01 by ADR-009

This ADR is **historical**. The product firmware is now the vendored
`esp32_nat_router` base built with ESP-IDF 5.5.x — see
[ADR-009](ADR-009-vendored-nat-router-base.md). ADR-009 also supersedes ADR-007
and ADR-008.

The reasoning below was correct given the evidence on 2026-09-29: the only proven
harness at the time was Arduino, and no working reference existed in the project.
The premise that changed is not the framework's merits — it is that a complete,
working router for this exact board already existed and was already running on the
device. Re-implementing it was the error.


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
Pros: all working baselines already run here, and PPP is already verified here
(TASK-020). Lowest immediate risk. Cons: ESP-SR and `esp_codec_dev` for I2S are
genuinely harder, since those are IDF components with no Arduino equivalent.

> **Corrected 2026-09-29.** This option originally listed NAT as a weakness. That
> was wrong and has been disproven by direct inspection of the shipped core; see
> the NAPT finding in the final section below. NAT is not a weakness of Option B.

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

---

## Update 2026-09-29 — PPP precondition satisfied, decision can be taken

TASK-020 is done. The first of the two preconditions named in the Recommendation
is now met, and it is met decisively:

- PPP negotiates end to end on the Airtel SIM. lwIP reaches phase `RUNNING` with
  `local=100.76.140.112`, `gateway=10.64.64.64`, `mtu=1500`, held stable, and
  reproduces on a cold boot. See TEST-020 and BUG-006.
- **RISK-002 is retired.** The premise that PPP was unreachable on this board is
  dead. It took three fixes, all in Arduino-land: a post-`CONNECT` settle delay,
  an explicit `tcpip_init()`, and the mandatory `LOCK_TCPIP_CORE()`.
- The original ESP-IDF "PPP fails" result was almost certainly the **Jio SIM**
  registration failure (`+CGREG: 0,3`), not a framework defect. It should be
  re-tested on the Airtel SIM before it is cited as evidence against ESP-IDF.

### What this does and does not change

It removes the strongest argument for ESP-IDF. The case for it was that
harder-to-port requirements are ESP-IDF-native: `esp32_nat_router` (REQ-004),
ESP-SR (REQ-018), `esp-mqtt` with TLS (REQ-013, REQ-022), and `esp_codec_dev`
for I2S (REQ-017). Of those:

| Requirement | Still an ESP-IDF advantage? |
|---|---|
| REQ-004 NAT routing | Yes, and now more important — but the PPP prerequisite it needed now works on Arduino, so the *feasibility* risk is gone even if the *convenience* argument stands |
| REQ-017 audio / I2S | **Unknown** — depends on RISK-003, still unanswered |
| REQ-018 ESP-SR | **Unknown** — same dependency, and ESP-SR descopes entirely if there is no mic |
| REQ-013/022 mTLS | Yes, but `esp-mqtt` is not the only path; Arduino has `ArduinoBearSSL`/mbedtls |

So the second precondition, RISK-003, now carries **both** remaining
framework-selection arguments. It is a single physical inspection of the board
and it is the cheapest high-leverage action available.

### Revised recommendation

**Option C, decided after RISK-003 is answered.** The shape of the answer barely
moves with the audio result:

- If I2S mic and amp **are** populated, ESP-IDF is the better product platform
  (ESP-SR, `esp_codec_dev`, `esp_mqtt` TLS are all real integration cost on
  Arduino), so Option C: ESP-IDF for product code, Arduino retained as the
  hardware regression harness.
- If they are **not** populated, REQ-017/REQ-019 descope, ESP-SR stops being an
  argument entirely, and the decision collapses to REQ-004 alone. NAT is a
  solved problem on ESP-IDF via `esp32_nat_router` but genuinely awkward on
  Arduino, so the answer is still likely ESP-IDF — just with a smaller
  migration, because the audio and speech work is no longer coming.

Either way the destination is the same, which is why this should not stay
blocked. What genuinely blocks TASK-101/103/104/108 is not the missing
information; it is that nobody has written the decision down.

**Proposed:** record Option C as Accepted, with the Arduino baselines retained
under `firmware/baseline/` as the regression harness, and revisit only if
RISK-003 descopes the audio work. TASK-019 closes on that basis.

---

## Update 2026-09-29 (later) — NAPT is in the Arduino core; decision is Option B

The proposal above is **not taken**. One more check overturned the premise it
rested on.

The load-bearing claim in the previous update was that "NAT is a solved problem on
ESP-IDF via `esp32_nat_router` but genuinely awkward on Arduino", and that
REQ-004 was the one remaining argument pulling the decision toward ESP-IDF. I
tested that claim against the actual shipped Arduino core rather than the
documentation, and it does not hold.

**NAPT is compiled in and exported.** From
`~/Library/Arduino15/packages/esp32/tools/esp32s3-libs/3.3.11/lib/liblwip.a`:

- `ar t` lists `ip4_napt.c.obj` — the module is in the archive, not merely
  present in a header.
- `nm` exports `ip_napt_enable`, `ip_napt_enable_no`, `ip_napt_enable_netif`,
  `ip_napt_forward` and `ip_napt_recv`.
- `libesp_netif.a` exports `esp_netif_napt_enable` and `esp_netif_napt_disable`,
  and `esp_netif_napt_enable(esp_netif_t *)` is declared in the shipped
  `esp_netif.h`.

And from the core's `sdkconfig`: `CONFIG_LWIP_IP_FORWARD=y`,
`CONFIG_LWIP_IPV4_NAPT=y`, `CONFIG_LWIP_IPV4_NAPT_PORTMAP=y`. The corresponding
`lwipopts.h` shows `IP_FORWARD 1`, `IP_NAPT 1`, `IP_NAPT_PORTMAP 1`.

So the last argument for ESP-IDF is gone. **REQ-004 is no longer a
framework-selection discriminator at all.** Everything this ADR listed as an
ESP-IDF advantage now reduces to audio and speech (REQ-017, REQ-018), which
depend on the still-unanswered RISK-003, plus TLS convenience that Arduino
covers with mbedtls.

### Two implementation facts worth keeping

Both were read out of the lwIP source, not assumed, and both will silently
mislead an implementer:

1. **NAPT keys off the inbound netif.** `ip_napt_forward()` opens with
   `if (!inp->napt) return ERR_OK;` — translation only happens if the netif the
   packet *arrived on* is NAPT-enabled. For SoftAP-client-to-PPP traffic that
   means enabling NAPT on the **SoftAP** netif and pointing the default route at
   the PPP netif. Enabling it on the outbound netif compiles, boots, and drops
   all client traffic with no error.
2. **`ip_napt_enable_netif()` is a silent no-op on a down netif** — it returns 0
   without enabling when `!netif_is_up(netif)`. Ordering is: AP up, then enable.

A ceiling to carry into TASK-110: `ip_napt_init()` heap-allocates a fixed
`IP_NAPT_MAX = 512` entry table (~16 KB, `mem_calloc`, not a memp pool), and
`ip_napt_init` is not an exported symbol, so the table cannot be enlarged without
rebuilding lwIP. That caps concurrent translated connections and is a candidate
cause for missing the 10–15 Mbps NFR (RISK-001).

## Decision

**Option B — Arduino-ESP32 3.3.x as the product framework.** The Arduino
baselines stay exactly where they are and stop being "a separate test harness":
they become the product's own foundations. ESP-IDF is not adopted now.

Rationale:

- The decisive argument for ESP-IDF is disproven. NAPT is present and exported in
  the shipped core.
- PPP is *already verified on Arduino* (TASK-020) and needed three non-obvious
  fixes to get there. Porting that to ESP-IDF means re-proving it and re-earning
  those fixes.
- Every one of the eleven hardware regression sketches is Arduino and all
  compile. One build system, one flash path, one serial console.
- REQ-004 is the product's headline requirement and it is now unblocked on the
  platform that already works.

Consequences:

- TASK-019 closes as `VERIFIED`. TASK-101, TASK-103, TASK-109, TASK-108 and
  TASK-110 are unblocked.
- The first NAT milestone is **LTE-only NAT** (TASK-101 + TASK-109), not dual-WAN
  failover. Proving NAPT translation over PPP is the hard part; STA and failover
  are layered on afterwards. Dual-WAN integration stays in TASK-104.
- RISK-003 (audio hardware) no longer blocks any network work. It still gates
  REQ-017/REQ-018 only. If the board turns out to have no I2S hardware, ESP-SR
  work is descoped, and the "ESP-SR is an IDF-only feature" argument disappears —
  which under this decision would simply reinforce Option B, not reopen it.
- `nomad-sentinel` stays a separate IDF project. Nothing is ported now. If audio
  work later proves to need ESP-SR, that is a fresh, evidence-based decision with
  a much narrower scope than migrating the whole product.
- `TASK-104` is retitled to "Dual-WAN routing integration" to remove its overlap
  with TASK-109, and `REQ-002` now points at TASK-102/TASK-105 rather than at the
  NAT tasks.
