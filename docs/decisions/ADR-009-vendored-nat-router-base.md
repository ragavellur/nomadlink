# ADR-009 — Product firmware is the vendored esp32_nat_router base (ESP-IDF)

- **Status:** ACCEPTED
- **Date:** 2026-10-01
- **Supersedes:** ADR-005 (Arduino-ESP32 3.x) and ADR-007/ADR-008 (web stack)
- **Related:** TASK-102, RISK-011, TASK-019

## Context

Two prototype firmwares were written for NomadLink's network stack:

1. An Arduino-ESP32 3.3.11 sketch (`firmware/nomadlink`, commit `daed4b3`) doing
   SoftAP + STA + PPP + NAPT with a hand-written console. It compiled, was gated,
   and reached the internet over PPP.
2. The user's own flash of **`esp32_nat_router` 2.4.17**, which the project owner
   reports works correctly on this board.

A prototype was then requested that reproduced, feature for feature, a system
which **already exists, is battle-tested, and is running on the device**. Building
it by hand re-implements `ip_napt_enable()` routing, the DHCP server, the packet
hooks, the web console and the client statistics — and reproduces the exact class
of bug this project already recorded: the Arduino port hung under forwarding load
because `ESPAsyncWebServer` drives lwIP from the wrong thread (ADR-008). That
defect exists because the reference uses ESP-IDF and `esp_http_server`; hand-rolling
the Arduino equivalent walked straight back into it.

ADR-005 chose Arduino on the evidence available on 2026-09-29, when the only
proven harness was Arduino. That premise is now false.

## Decision

The product firmware is **vendored upstream `esp32_nat_router` source**, built with
ESP-IDF, at `firmware/nat_router`. NomadLink features are added **into that tree**,
not alongside it.

- Upstream: `https://github.com/martin-ger/esp32_nat_router`, version 2.4.17,
  commit `2fe7a4c`, vendored verbatim.
- Toolchain: **ESP-IDF 5.5.x**. Verified building for `esp32s3` at
  `esp32_nat_router.bin binary size 0x14f970` (1,374,576 bytes).
- The base must compile **unmodified** before any feature is layered on. A build
  that only succeeds after local edits is not the base.
- Routing stays native lwIP: `CONFIG_LWIP_IP_FORWARD=y`,
  `CONFIG_LWIP_IPV4_NAPT=y`, `ip_napt_enable(my_ap_ip, 1)`, bundled `dhcpserver`.
- `CONFIG_LWIP_L2_TO_L3_COPY=y` stays set — the packet hooks need it. It is not a
  NAT switch.

The Arduino sketch is retained at `firmware/nomadlink` only as historical evidence
and is no longer the product.

## Consequences

- Forwarding, NAT, DHCP, console, ACL, WireGuard, port mapping and per-client
  statistics are inherited proven rather than re-implemented.
- **The toolchain is pinned to 5.5.x.** The base does not build on IDF 6.x: `main`
  requires component `json`, which moved into the component manager
  (`Failed to resolve component 'json' required by component 'main': unknown
  name`). This is a toolchain mismatch, not a code fault — do not "fix" it by
  editing the component list, which would diverge from the base.
- The Arduino FQBN constraint (`app3M_fat9M_16MB`) and the Arduino-only flashing
  procedure no longer describe the product. Upstream's partition table
  (`partitions_example.csv`, 4 MB layout) does.
- `make product` now gates `firmware/nat_router` and skips cleanly when no 5.5.x
  install is present, so a missing toolchain cannot masquerade as a pass.
- **RISK-011 is open:** upstream declares no licence. See
  `docs/project/risks.json`.
- The web installer at `docs/project/index.html` publishes the base image; it is
  explicitly labelled as base-only, with the 4G/GNSS/camera features absent.

## Verification

| Claim | Evidence |
|---|---|
| Vendored source is unmodified upstream | `firmware/nat_router/README.md` provenance table; `git diff` against upstream clean |
| Base compiles for ESP32-S3 | `make product` → `nat_router ok (ESP-IDF 5.5.4, app binary size 0x14f970 / 1374576 bytes)` |
| The compile gate rejects a broken tree | Injected invalid C into `main/portmap.c` → gate exits 1 naming the file and line; reverted, tree builds again |
| Web installer renders and its payload resolves | `esp-web-install-button` defined, manifest attribute set, button visible; HTTP 200 for manifest and all four parts |
| Router behaviour on hardware | **NOT TESTED from this repository.** Upstream and the project owner report the image working on this board |