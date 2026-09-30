# ADR-008 — Admin console: core `WebServer`, and a selectable uplink

- **Status:** ACCEPTED
- **Date:** 2026-09-30
- **Supersedes:** ADR-007 (decision 1 only — the async web server choice)
- **Related:** TASK-801, TASK-802, TASK-101, TASK-102, TASK-404, REQ-001, REQ-020, REQ-022, ADR-005

## Context

ADR-007 chose `ESPAsyncWebServer` on a spike that **compiled**. It then
boot-looped on hardware. The difference between those two facts is the whole
reason this ADR exists, so it is worth stating precisely.

`ESPAsyncWebServer` pulls in `AsyncTCP`, which bypasses the socket layer and
calls raw lwIP: `tcp_listen_with_backlog()` → `tcp_alloc()`. The Arduino core
builds lwIP with `CONFIG_LWIP_CHECK_THREAD_SAFETY=y`, which arms
`LWIP_ASSERT_CORE_LOCKED()` on every raw lwIP entry point, and with
`CONFIG_LWIP_TCPIP_CORE_LOCKING_INPUT` **not** set, so that assert only passes
on the tcpip thread itself. `"Required to lock TCPIP core functionality!"`
appears 21 times in the shipped `liblwip.a`.

Three arrangements were tried on hardware. All three fail, and the reason is
that the restriction lives in the **core's lwIP build**, not in scheduling:

| Attempt | Result |
|---|---|
| `begin()` from the Arduino setup task | `abort()`, boot loop |
| `begin()` wrapped in `LOCK_TCPIP_CORE()` | Deadlock — `AsyncTCP` uses a *blocking* `tcpip_api_call()` and the tcpip thread needs the lock we hold |
| `begin()` from a dedicated 4 KB FreeRTOS task | Same assert, different stack in the backtrace — a new task is not a new thread identity |

The `esp32_nat_router` reference is immune for a reason that is easy to miss:
its `components/http_server` is built on **`esp_http_server`**, not on
`ESPAsyncWebServer`. It never calls raw lwIP either. The reference was never
evidence for `ESPAsyncWebServer`; it was evidence for "do not use the one API
that trips the assert".

One correction to ADR-007's framing: `LWIP_TCPIP_CORE_LOCKING_INPUT` defaults to
**`n`** in ESP-IDF, so the core's value was not a deviation from a default that
would have been safer. Nothing about the default would have saved this.

The second half of this ADR is a scope change: the device is a **travel gateway
with two uplinks**, and ADR-007 left the uplink hard-wired to PPP.

## Decision

**Core `WebServer.h`, direct-IP access, auth deferred, uplink selectable.**

1. **`WebServer.h` (core) replaces `ESPAsyncWebServer`.** It reaches lwIP through
   `NetworkServer`, i.e. the BSD socket layer, which takes the tcpip lock
   itself. It is *not* `esp_http_server`; that distinction is recorded because
   conflating the two has already produced one wrong explanation here.
2. **The cost is stated, not hidden: the core `WebServer` is poll-driven, not
   async.** In 3.3.11 `_server` is a `NetworkServer` and `handleClient()` runs
   the whole accept/parse/respond state machine for **one connection at a time**.
   It **must** be called from `loop()` or the listener accepts nothing and the
   console silently never loads — a failure indistinguishable from a dead AP.
   Its `_nullDelay` defaults to true, so each idle pass costs `delay(1)`. This
   is a real regression against ADR-007's async premise and is accepted: the
   alternative is a boot loop.
3. **A scan is a blocking call inside `handleClient()`.** `WiFi.scanNetworks()`
   occupies the radio for seconds, so the console stalls while it runs. The AP
   keeps serving, and the reply is the real scan result, not a cached list.
4. **Serve at `http://192.168.4.1` and `http://nomadlink.local`**, auth deferred
   to TASK-404. Unchanged from ADR-007.
5. **The uplink is selectable: 4G (PPP) or Wi-Fi (STA).** This is the
   `esp32_nat_router` model — an AP netif for clients, candidate uplink netifs,
   and `netif_set_default()` deciding where forwarded traffic goes. lwIP routes
   and NAPT translates in both cases; **no route or NAT rule is hand-rolled.**
   The choice is stored in NVS and reapplied on boot.
6. **NAPT stays enabled on the AP netif regardless of uplink.** Client packets
   always enter through the AP, so translation happens on the way in. Switching
   uplinks therefore does not touch it.

## Rules the implementation must keep

ADR-007's rules all still hold and are restated only where this decision changes
them. New or changed ones:

- **Selecting an uplink with no address is refused.** Moving the default route
  to a dead interface black-holes every client, so `/api/uplink/mode` rejects
  `wifi` unless the STA holds a DHCP address, and a dropped STA must not be left
  as the default.
- **The console must report the default route from the routing table, not from
  the modem.** Reading `netif_ip4_gw()` from the PPP netif inside the PPP-up
  branch made the page keep advertising the PPP peer as the "default route"
  after a switch to WiFi — a label that followed the modem instead of the
  routing table. It is read from `netif_default` unconditionally.
- **`esp_netif_get_netif_impl()` must be declared with `extern "C"`.** It is
  exported (defined in `libesp_netif.a`) but absent from `esp_netif.h`. A plain
  declaration in a `.ino` mangles the call to
  `_Z24esp_netif_get_netif_implP13esp_netif_obj` and the link fails with an
  "undefined reference" to a name that visibly exists in the archive. The
  reference never hits this because `netif_hooks.c` is C. This is the sixth
  instance of the L-14 family: grep the archive *and* the headers before writing
  a dependency conclusion into a document.
- **Do not reach into lwIP internals for display.** `netif_name()` exists only in
  `lwip/src/include/lwip/netif.h`. The interface label is derived by pointer
  identity against the netifs this firmware owns.
- **Credentials live in the gateway's own NVS, never in the repository.** That
  store is plaintext and unencrypted; the gap is tracked with the
  credential-storage task and stated on the page rather than implied secure.

## Consequences

- TASK-102 (Wi-Fi uplink) becomes implementable on this platform.
- The console is **more** responsive-limited than ADR-007 promised and **less**
  crash-prone. That trade is deliberate: a boot loop is not a performance
  problem.
- App partition usage is 997,971 B of 3 MB. Camera streaming and the MQTT client
  are still to come; headroom is a tracked constraint.
- The DHCP pool is **11 addresses** (`CONFIG_LWIP_DHCPS_MAX_STATION_NUM=8`).
  Unchanged, and still stated on the page.
- Still open and still needing hardware: per-client traffic accounting
  (TASK-110) and the WAN failover state machine (TASK-108).