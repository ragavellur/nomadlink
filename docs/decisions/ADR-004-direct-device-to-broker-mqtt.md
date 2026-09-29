# ADR-004 — Direct device-to-broker MQTT, no relay

- **Status:** Accepted
- **Date:** 2026-09-29
- **Requirements affected:** REQ-012

## Context

An early approach published device telemetry by running a Python bridge on the
development Mac: the ESP32 posted to a local HTTP endpoint, the bridge
published to the user's HiveMQ Cloud cluster over WSS, and cloudflared or ngrok
exposed the local port.

This worked in the lab but is rejected for several reasons:

- It depends on a developer machine being online. The product is a travel
  gateway; it must work with no PC attached.
- It introduces an unauthenticated HTTP surface on the public internet.
- Latency and availability become a function of somebody's laptop.
- It is a security anti-pattern that cannot ship to a customer.

The user has explicitly and repeatedly stated the requirement: **telemetry must
go directly from the device to the broker.**

## Decision

The device publishes directly to the broker over the cellular data path.
No bridge, tunnel, reverse proxy, relay, or cloudflared in the data path, ever.

The transport is the A7670E's own TCP/IP stack driven over AT commands, using
`AT+CIPOPEN` / `AT+CIPSEND`. `AT+CIPSTART` is **not** used: it returns `ERROR`
on this firmware. `AT+CIPOPEN` is what the TinyGSM A7672x client uses internally
and is proven to open sockets (`+CIPOPEN: 0,10`).

The product's HTTP surfaces (web portal, camera stream) remain on the device's
own SoftAP. They are a management interface on the local network, not an
internet-facing relay.

## Alternatives considered

- **Keep the bridge as a dev convenience behind a flag.** Rejected: the user was
  unambiguous, and a flag that ships invites accidental dependence.
- **Use the ESP32-S3 Wi-Fi uplink for MQTT instead of LTE.** Rejected as the
  primary path: cellular is the product's reason for existing. Wi-Fi uplink
  remains useful as a development transport.
- **Use modem PPP plus a normal TCP/IP stack, then a normal MQTT client.**
  Attractive and preferred if RISK-002 resolves, because it would give TLS and
  would remove the need to hand-roll MQTT framing. Currently blocked because
  PPP has never produced an IP on this device.

## Consequences

- The MQTT packet handling is ours, including the two known parsing defects in
  BUG-002. This is real complexity and is tracked as TASK-405.
- Migrating to the TinyGSM A7672x client is attractive precisely because it
  already implements the `CIPOPEN` path correctly. Evaluated in TASK-019.
- TLS is harder over a hand-driven `CIPSEND` socket than over a normal TCP
  stack. RISK-005 is the associated security risk and is a P1.
- Verification of REQ-012 must include an **independent subscriber** observing
  the payload. A self-reported send is not evidence.
