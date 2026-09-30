# ADR-007 — Admin console: async web server, direct-IP access, deferred auth

- **Status:** ACCEPTED
- **Date:** 2026-09-30
- **Related:** TASK-801, TASK-802, TASK-101, TASK-404, REQ-001, REQ-020, REQ-022, ADR-005

## Context

The product had no HTTP surface at all before this ADR. `firmware/nomadlink`
brought up the SoftAP, DHCP and PPP, and printed status to the UART, but an
admin had no way to see device state without a serial cable. REQ-020 asks for a
device web UI; the PRD names `martin-ger/esp32_nat_router` as the reference for
both the web interface and NAT behaviour.

ADR-005 settled the product framework as **Arduino-ESP32 3.3.11**, not ESP-IDF.
That makes the reference project's framework a non-starter as a dependency: it is
an ESP-IDF component, and its NAT half is already reimplemented in the Arduino
sketch. So the reference contributes **UI and interaction patterns only**, not
code. Two things follow that this ADR has to settle.

### 1. Which web server

Core 3.3.11 ships `WebServer.h`, which is blocking and single-threaded. The
camera stream, MQTT and the PPP feeder are all competing for the same task
budget, and a blocking handler serving a page during a camera capture is a
latency bug waiting to happen. `ESPAsyncWebServer` is the usual answer, but
"usual" is not evidence.

**Measured before deciding, not assumed:**

| Check | Result |
|---|---|
| `ESPAsyncWebServer 1.2.4-fork` + `AsyncTCP 1.1.4` on core 3.3.11, production FQBN | Compiles |
| Flash / RAM cost of a throwaway spike | 948,319 B (30%) / 47,300 B (14%) |
| Product sketch with the console inlined | 989,467 B (32% of the 3 MB app partition) |

The spike initially failed, and the failure was worth recording: it was
`ESP.getChipId()`, an API that does not exist in this core. The libraries
themselves were never the problem. This is the third time in this project that a
compile error in a *new* test code was mistaken for a hardware or dependency
fault — see L-14.

### 2. How a client reaches the page

A captive portal was the original assumption, and on this firmware it **cannot
work**: lwIP's DNS server is compiled out of the Arduino core
(`CONFIG_LWIP_DNSSERVER` is off), so DHCP clients are handed public resolvers and
an OS connectivity probe reaches the real internet instead of the gateway. There
is no interception point and adding one means building a DNS server, which is
real work for a convenience feature.

So the console is served at a known address and the user is told the address.

## Decision

**Option 1, adopted — `ESPAsyncWebServer`, direct-IP access, auth deferred.**

1. **`ESPAsyncWebServer` + `AsyncTCP` over the core `WebServer.h`.** Non-blocking
   handlers keep the page responsive while the PPP feeder and any future camera
   or MQTT task are running. The core `WebServer` is the documented fallback: if
   the async libraries ever break a core upgrade, the console degrades to a
   blocking single-client server rather than disappearing.
2. **Serve at `http://192.168.4.1` and `http://nomadlink.local`.** mDNS via the
   built-in `ESPmDNS`, no DNS server and no portal interception. The exact address
   is printed on the UART at boot *and* shown in the UI, so there is no discovery
   problem to solve.
3. **Authentication is deferred to TASK-404 and the console is explicitly not
   production-ready until it exists.** The UI says this in the Network and
   Telephony sections rather than hiding it.
4. **The page is fully self-contained.** No CDN, no web fonts, no remote CSS/JS.

### Rules the implementation must keep

These are not aspirations; each one is a way this project has previously
produced a false result.

- **The web task must never touch the UART.** The PPP feeder owns it for the
  life of the session. Modem telemetry is sampled in the one safe window before
  the dial, then cached, and the UI shows the **age of the sample**. With PPP up
  the CSQ number on screen is explicitly a snapshot, not a live reading, because
  the alternative — an interleaved `AT+CSQ` — corrupts live LCP traffic.
- **No page may issue AT commands, and no page may be reached before the SoftAP
  is up.** `startConsole()` is called immediately after `startSoftAP()` returns.
- **Report only what was measured.** A field with no measurement says NOT
  INTEGRATED and names the TASK that owns it. A plausible zero is how this
  project shipped two false "unsupported" verdicts, so a client that has
  associated but has no lease shows `no address yet` — the real core behaviour —
  and never a borrowed or invented address.
- **Client MAC/IP pairs come from `esp_wifi_ap_get_sta_list_with_ip()`.** Note
  that `WiFi.softAPgetStationInfo()` was removed in core 3.x. Inferring a MAC
  from the `STAIPASSIGNED` event would be a heuristic, and a wrong pairing shown
  as fact is exactly the bug class this project exists to prevent.
- **No section shows a number that has not been re-measured.** Location shows no
  position at all while BUG-009 and BUG-010 are open.

## Consequences

- TASK-801 and TASK-802 become implementable on the platform already in use;
  TASK-101's manual test step is unblocked.
- The product sketch is now inside the commit gate. `build-baseline.sh` loops
  `firmware/baseline/*` only, so a 1,300-line product sketch had **no compile
  gate at all** and could rot silently. `scripts/project-tracker/build-product.sh`
  and `make product` close that hole, and `make check` now depends on it.
- The console is a **test surface, not a production one.** No auth, no CSRF
  consideration, no rate limiting, no TLS. It is only ever reachable over the
  device's own SoftAP, and TASK-404 must close before this is treated as a
  shipped admin surface.
- 32% of the app partition is consumed. Camera streaming and the MQTT client are
  still to come, and partition headroom is now a tracked constraint.
- The DHCP pool is **11 addresses** (`CONFIG_LWIP_DHCPS_MAX_STATION_NUM=8` in this
  core), not the 253 a client expects. The console says so rather than implying
  a normal-size pool. Widening it is a core rebuild, not a config change.
- Two open items this ADR does **not** settle, both needing hardware:
  AP-to-PPP forwarding with `CONFIG_LWIP_L2_TO_L3_COPY` absent, and per-client
  traffic accounting (TASK-110).
