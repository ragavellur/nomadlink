# ADR-002 — Verified hardware facts override the PRD

- **Status:** Accepted
- **Date:** 2026-09-29
- **Requirements affected:** REQ-003, REQ-007, REQ-008, REQ-012, REQ-023

## Context

The Product Requirement Document v2.0 was written as a specification for a
class of device, not from measurements of *this* device. In several places it
asserts things that are false for the actual Waveshare ESP32-S3-A7670E-4G
hardware now on the bench, on firmware `A011B04A7670M7_F`, with an Airtel SIM.

The following PRD statements are contradicted by physical evidence.

| PRD statement | Measured reality |
|---|---|
| FR-3.1: use `AT+CGNSINF` | Command does not exist. `+CME ERROR`. `AT+CGPSINFO` is valid. |
| FR-1.3: PPP Mode | PPP has never produced an IP on this device. See RISK-002. The working path is the modem's own TCP/IP stack via `AT+CIPOPEN`. |
| Hardware table: I2S MEMS mic + I2S DAC | Presence and wiring unconfirmed on this board. nomad-sentinel records the audio path as UNSUPPORTED. See RISK-003. |
| NFR 7.1: 10–15 Mbps NAT throughput | Modem is Cat-1 bis. Not measured, and unlikely to be achievable alongside video and DSP. See RISK-001. |
| NFR 7.2: speech recognition under 800 ms | Never benchmarked. See RISK-004. |
| NFR 4.4: TLS 1.2/1.3 for cloud | Untested on this firmware; only plaintext 1883 is proven. See RISK-005. |
| Storage: NVS / LittleFS | SD *is* present and specified in the hardware table, and works under Arduino but fails under ESP-IDF. See RISK-006 / BUG-001. |

Conversely, the PRD **omits a requirement that turns out to be a real, working
product capability**: cellular LBS position fallback. `AT+CLBS=1,1` returns
`+CLBS: 0,18.481703,73.897415,550`, independently corroborated by the GNSS fix.
This is recorded as REQ-008.

There is also a documented history of a previous agent asserting a capability
was absent based on a small number of failed invocations, and of `+CME ERROR`
from a non-existent command being misread as a hardware fault. That is the
failure mode this ADR exists to prevent.

## Decision

**Where the PRD and a physical measurement disagree, the measurement wins, and
the PRD is corrected.** The PRD is treated as a statement of intent, not as an
authoritative description of the hardware.

Specifically:

1. Every requirement carries a `hardware_verified` field and a
   `verification_note` in `docs/product/requirements.json`. A requirement with
   no physical evidence behind it is marked `NOT_STARTED`, never `VERIFIED`.
2. `AT+CMD=?` is run before forming any opinion about a command's capabilities.
   `AT+CLBS=?` is what revealed the mandatory CID argument that four earlier
   failed attempts had missed.
3. `+CME ERROR` from an unverified command name is **not** evidence of a
   hardware fault. Check the command set first.
4. Signal state (`AT+CSQ`) and physical precondition state (such as whether the
   4G patch antenna is attached) must be logged in the same run as any result
   being used as evidence. Results spanning different hardware states are not
   comparable.
5. A `VERIFIED` label requires the exact command, the exact observed output, and
   a commit or log reference. It never means "the code compiled".

## Alternatives considered

- **Treat the PRD as binding and debug the hardware until it matches.** Rejected:
  it would have meant chasing a non-existent AT command and an unreachable
  throughput target, and would have repeated the exact error that cost a
  working feature three rounds of the user's time.
- **Silently correct the PRD without recording it.** Rejected: the discrepancy
  would be invisible to the next reader, and the original requirement would
  appear to have always been correct.
- **Await product clarification before planning.** Rejected: enough is verified
  to plan the foundation, the location service, and the MQTT transport, which
  are the P0 spine. The blocked areas are explicitly marked rather than guessed.

## Consequences

- The PRD must be amended. FR-3.1 needs `AT+CGPSINFO`. FR-1.3's PPP assumption
  needs to become conditional on RISK-002 resolving. The audio row in the
  hardware table needs to reflect the real BOM. NFRs 7.1, 7.2 and 4.4 need
  measurement or renegotiation.
- REQ-008 (LBS fallback) is a new requirement not traceable to the PRD. It is
  flagged as such in `requirements.json`.
- Sprints must be ordered so that RISK-001, RISK-002 and RISK-003 are measured
  early, before dependent work is built on top of unvalidated NFRs.
- Verification evidence is now a first-class tracked artefact rather than a
  claim in a status table.

## Security impact

This ADR directly prevents a category of defect where plaintext credentials or
an insecure transport were treated as acceptable because an NFR was believed to
be satisfied. It also mandates that `tracker/tracker.ino`'s hardcoded broker
credential (BUG tracked in TASK-004) be removed before the file is committed.

## Operational impact

An operator must know the antenna state and signal strength for any reported
position or telemetry failure. The dashboard should surface both, so that
"it stopped working" can be diagnosed without re-flashing a diagnostic sketch.
