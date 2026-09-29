# ADR-006 — Location source hierarchy: LBS first, then GNSS upgrade

- **Status:** Proposed
- **Date:** 2026-09-29
- **Requirements affected:** REQ-007, REQ-008

## Context

Two independent position sources are proven on this hardware:

| Source | Command | Result | Latency | Accuracy |
|---|---|---|---|---|
| GNSS | `AT+CGNSSPWR=1`, `AT+CGNSSPORTSWITCH=1,1`, `AT+CGNSSTST=1` | `18.480776, 73.897998` | 69 s cold start, unbounded warm | Metres |
| Cellular LBS | `AT+CLBS=1,1` | `+CLBS: 0,18.481703,73.897415,550` | Seconds | 550 m radius |

The two agree to well within the LBS accuracy radius, which is the cross-check
that makes the LBS figure credible.

The PRD describes only GNSS. LBS is an addition found during hardware testing.

An important ordering constraint: GNSS cold start **must not be interrupted**.
The module needs one uninterrupted power-on to download a fresh almanac. This
is documented in `LESSONS_LEARNED.md`, including the specific failure signature
of an unpowered engine (empty C/N0, empty azimuth, drifting elevation, GGA
quality 0) which mimics "no sky view."

## Decision

The location service publishes a single position record with an explicit source
and accuracy:

```json
{
  "lat": 18.481703,
  "lon": 73.897415,
  "source": "LBS",
  "accuracy_m": 550,
  "sats_used": 0,
  "timestamp": "..."
}
```

Behaviour:

1. **At boot, immediately query LBS** and publish a position within seconds. A
   travel gateway that shows nothing for 69 seconds on first power-up looks
   broken, and indoors the GNSS may never fix at all.
2. **Concurrently start the GNSS engine once** and keep it powered. Do not
   power-cycle.
3. **When the GNSS fix arrives and quality >= 1 with sats_used > 0**, replace
   the position, set `source` to `GNSS`, and set `accuracy_m` from the HDOP
   derived value.
4. **Consumers must always see `source` and `accuracy_m`.** A consumer that
   renders a 550 m fix identically to a 3 m fix is misleading the user. The web
   map must visually distinguish them, and MQTT telemetry must carry both fields.

Preconditions for LBS, both of which must be checked and reported:

- The **4G patch antenna is attached.** Without it, `+CSQ` drops from 26 to 16
  and `AT+CLBS` returns `+CLBS: 9`.
- **`AT+SIMEI` is populated** from `AT+CGSN`. Unpopulated gives `+CLBS: 12`.

## Alternatives considered

- **GNSS only, per the PRD.** Rejected: unusable indoors and slow at boot, and it
  discards a working capability.
- **LBS only.** Rejected: 550 m is not accurate enough to be the primary source
  for a tracking product.
- **Try GNSS with a long timeout, fall back to LBS on failure.** Rejected: this
  inverts the priority and leaves the user staring at a blank map for the whole
  cold start. Publishing the coarse fix immediately and upgrading it is strictly
  better.

## Consequences

- The product gains a position almost immediately after power-up, which is a
  real UX improvement over the PRD's design.
- The dashboard must show source and accuracy, or it will misrepresent the data.
- `AT+CLBS=1` **without a CID argument must never be used**; it defaults to an
  unusable context and returns `+CLBS: 9`. This is the specific defect that
  caused a working feature to be declared broken.
- Antenna state becomes an operator-visible diagnostic, because forgetting it
  silently breaks the fallback.

## Security impact

None directly. Note that LBS transmits cell identity to the carrier's location
service as an inherent part of the feature; this should be disclosed in the
privacy section of the product documentation.
