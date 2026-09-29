# NomadLink

**NomadLink One** · ESP32-S3 + SIMCom A7670E · **NomadLink OS** firmware · **NomadLink Console** web portal · **NomadLink Cloud** telemetry

An SDLC-tracked travel gateway: dual-WAN failover routing, camera streaming,
GNSS location with cellular fallback, direct MQTT telemetry, SMS commands and
telephony.

> **Status: foundation stage.** The hardware capabilities below are physically
> verified. The product software has not been started. See
> [`docs/product/product-spec.md`](docs/product/product-spec.md) and the
> dashboard at [`docs/project/index.html`](docs/project/index.html).

---

## Project dashboard

Generated from JSON. Open locally or view on the GitHub-hosted URL.

| Page | Contents |
|---|---|
| [`docs/project/index.html`](docs/project/index.html) | Overview, honest status, requirement traceability |
| [`docs/project/sprint-board.html`](docs/project/sprint-board.html) | Kanban board across all states |
| [`docs/project/requirements.html`](docs/project/requirements.html) | All 23 requirements with verification evidence |
| [`docs/project/tests.html`](docs/project/tests.html) | Test results — a PASS without evidence is rejected |
| [`docs/project/bugs.html`](docs/project/bugs.html) | Open defects with root cause |
| [`docs/project/risks.html`](docs/project/risks.html) | Risk register |
| [`docs/project/releases.html`](docs/project/releases.html) | Release readiness checks |
| [`docs/project/changelog.html`](docs/project/changelog.html) | Activity log |

## Repository layout

```
docs/
  product/      product-spec.md, requirements.json, epics.json
  architecture/ architecture and design documents
  decisions/    ADRs — every significant decision, with rationale
  testing/      test-strategy.md
  project/      JSON source of truth + generated HTML dashboards
firmware/
  baseline/     hardware regression sketches (Arduino)
  nomadlink/    product firmware (framework per ADR-005)
scripts/project-tracker/
  validate.py       enforces the tracking invariants; non-zero on bad state
  render.py         generates the HTML dashboards from JSON
  secrets_scan.py   fails if a credential reaches git
```

## Working commands

```bash
# Validate project state. Non-zero if a task is DONE without evidence/commit,
# a VERIFIED claim has no measurement, or any ID reference is orphaned.
python3 scripts/project-tracker/validate.py

# Regenerate the HTML dashboards after changing any JSON.
python3 scripts/project-tracker/render.py

# Fail if a credential is in the working tree. Add --history to scan all commits.
python3 scripts/project-tracker/secrets_scan.py
python3 scripts/project-tracker/secrets_scan.py --history

# Full gate
make check
```

`make check` runs the validator, the secrets scan and a dashboard regeneration.
It must pass before any commit.

## Verified hardware baseline

These are physical measurements, not targets.

| Capability | Result |
|---|---|
| GNSS | `18.480776, 73.897998` — 69 s cold start, 17 GPS + 10 GLONASS |
| Cellular LBS fallback | `+CLBS: 0,18.481703,73.897415,550` — agrees with GNSS |
| LTE data | `+CEREG: 0,1`, `+CGPADDR: 1,100.90.93.199`, `+CSQ: 26,99` |
| Broker socket | `AT+CIPOPEN` → `+CIPOPEN: 0,10` |
| Camera | OV2640 live capture, luma stddev 15.4 |
| SD card | 3.98 GB FAT32 read/write/delete *(Arduino path only)* |
| SMS out | `+CMGS: 2`, `+CMGS: 3`, received |
| Voice out | `+VOICE CALL: BEGIN`, `+CLCC: ... 2` |

### Operating preconditions

- **Attach the 4G patch antenna.** Without it `+CSQ` drops 26 → 16 and LBS
  returns `+CLBS: 9`.
- **Populate `AT+SIMEI`** from `AT+CGSN`, or LBS returns `+CLBS: 12`.
- **Never power-cycle the GNSS engine.** It needs one uninterrupted power-on to
  download an almanac.
- Use `AT+CIPOPEN`, never `AT+CIPSTART` (returns `ERROR` on this firmware).
- Use `AT+CLBS=1,1`, never `AT+CLBS=1` — the CID is mandatory in practice.

Full details and the reasoning behind each: [`LESSONS_LEARNED.md`](LESSONS_LEARNED.md).

## Not yet proven

- End-to-end MQTT publish from device to broker (BUG-002, TEST-403 FAIL)
- NAT routing / PPP data path (blocked, RISK-002)
- Web portal (not started)
- Audio hardware presence (RISK-003)
- TLS on the cellular path (RISK-005)

## Secrets

No credential belongs in this repository. Broker credentials are read from
`secrets.h` (gitignored). Copy the template:

```bash
cp firmware/baseline/tracker/secrets.h.example firmware/baseline/tracker/secrets.h
```

A credential that has been committed or shared in plaintext must be **rotated**,
not merely removed. `secrets_scan.py` is a backstop, not a substitute.

## How work is tracked

Every task moves through:
`BACKLOG → READY → IN_PROGRESS → CODE_REVIEW → TESTING → VERIFICATION → DONE`

`DONE` requires all of: acceptance criteria satisfied, tests actually run,
evidence recorded, a real commit SHA, and a passing validator. Code compiling is
not verification. Details in [`docs/testing/test-strategy.md`](docs/testing/test-strategy.md).
