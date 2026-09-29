# NomadLink — Test Strategy

## Purpose

Define what "tested" and "verified" mean for this project, and what evidence is
required before a task may be marked `DONE`.

The dominant risk in this project is not functional bugs. It is **false
confidence**: a capability reported as working, or reported as broken, when the
evidence did not support it. This strategy is weighted accordingly.

## Test pyramid for embedded firmware

| Level | What it covers | Applies to |
|---|---|---|
| Host unit | Pure logic: parsers, formatters, state machines, command dispatch | NMEA parsing, MQTT JSON build/parse, SMS command matching, failover state machine |
| Target smoke | Does the firmware boot and reach a known state | Every firmware build |
| Hardware regression | Does each proven capability still work | GNSS, LBS, camera, SD, SMS, voice, data bearer |
| Integration | Do two subsystems work together | MQTT publish while GNSS runs; NAT failover while camera streams |
| End-to-end | Full user-visible path | Web UI action → hardware effect → broker receives → independent subscriber confirms |
| Security | Negative tests | Unauthorized SMS sender rejected; wrong Basic Auth rejected; no secrets in git |
| Performance | Against NFR targets | NAT throughput, failover time, speech latency |

Not every category applies to every task. **The decision must be explicit** —
a task that skips a level records why.

## Definition of Done (test-related clauses)

A task reaches `DONE` only when all applicable items hold:

1. Code implemented and formatted.
2. Static checks pass where a toolchain exists.
3. Unit tests written and passing, for anything with logic.
4. Hardware regression re-run if the change could touch a proven capability.
5. Acceptance criteria individually satisfied, each with a recorded result.
6. Documentation updated.
7. Git commit created and SHA recorded.
8. Verification evidence recorded in `backlog.json` **and** in
   `test-results.json`.
9. Project JSON and HTML updated and committed together.
10. No known blocking defect.

**Code compiling is not verification. A device printing a success marker without
a captured log is not evidence.**

## Evidence requirements

Evidence must be one of:

- A captured serial log excerpt, committed under `artifacts/` or pasted into
  `test-results.json`.
- An independent observer's confirmation (e.g. "user received the SMS", "broker
  subscriber received the payload").
- A measurement with the method stated.

Not acceptable as evidence:

- "It compiled."
- "The code looks right."
- "The test passed" without a captured result.
- A marker the code prints unconditionally. *(TASK-405's `MARK-CONNECTED` is
  exactly this defect: it prints regardless of whether the MQTT CONNECT
  succeeded.)*
- Values that are physically impossible. *(An earlier NMEA parser reported
  `satsUsed=99`, `hdop=0.00` — that was a parser bug, and it was nearly
  reported as a device fault.)*

## Hardware regression baseline

These are the proven capabilities. Re-run the corresponding sketch after any
firmware change that could plausibly affect them.

| Capability | Sketch | Success signal | Preconditions |
|---|---|---|---|
| GNSS fix | `firmware/baseline/gnss_hold` | Fix within a few minutes, quality 1, sats_used > 0 | 4G patch antenna attached, open sky, uninterrupted power-on |
| Cellular LBS | `firmware/baseline/lbsmatrix` | `+CLBS: 0,<lon>,<lat>,<acc>` | **4G patch antenna attached**, `AT+SIMEI` populated |
| Data bearer | `firmware/baseline/lbsmatrix` | `+CGPADDR: 1,<non-empty ip>` | SIM attached and unlocked |
| Camera | `firmware/baseline/cam_diag` | Non-zero luma variance | — |
| SD card | `firmware/baseline/sd_diag` | Write/read/delete round trip | Known-good FAT32 card |
| SMS out | `firmware/baseline/sms_test` | `+CMGS: <ref>` and user confirms receipt | `AT+CNMI=2,1,0,1,0` for delivery reports |
| Voice out | `firmware/baseline/voice_sms` | `+VOICE CALL: BEGIN`, `+CLCC: ... 2` | — |
| Broker socket | `firmware/baseline/tracker` | `+CIPOPEN: 0,10` **and** an independent subscriber receives a payload | Never assert success on `MARK-CONNECTED` alone |

## Rules for embedded verification

1. **Run `AT+CMD=?` before concluding a command is unsupported.** `AT+CLBS=?`
   revealed the mandatory CID argument after four failed attempts led to a
   working feature being declared broken.
2. **`+CME ERROR` from an unverified command name is not a hardware fault.**
3. **Log `AT+CSQ` and antenna state in the same run as any evidence.** The 4G
   patch antenna changed `+CSQ` from 16 to 26 and flipped LBS from failure to
   success. Results across differing hardware states are not comparable.
4. **Use `TinyGPSPlus`, not hand-rolled NMEA field indexing.** GGA lat/lon are
   two fields each (value + hemisphere), so indices shift by 4.
5. **Never power-cycle the GNSS engine mid-session.** It needs one uninterrupted
   power-on to download a fresh almanac.
6. **Do not use `AT+CIPSTART`.** It returns `ERROR` on this firmware. Use
   `AT+CIPOPEN`.
7. **Use `AT+CLBS=1,1`, never `AT+CLBS=1`.** The CID is mandatory in practice.
8. **A line-based serial reader cannot consume MQTT CONNACK.** Broker bytes
   (`20 02 00 00`) and the `AT+CIPSEND` `>` prompt are not newline-terminated.
   Read bytes.

## CI

Automated where possible:

| Check | Tool | Blocking |
|---|---|---|
| JSON schema/parse validity | `python3 -m json.tool` | Yes |
| Tracker invariants (Rule 44) | `scripts/project-tracker/validate.py` | Yes |
| No secrets in tracked files | `scripts/project-tracker/secrets_scan.py` | Yes |
| ESP-IDF build | `idf.py build` | Yes, once framework is chosen |
| Arduino sketch compile | `arduino-cli compile` per sketch | Yes, for baseline sketches |
| Markdown link check | manual / CI | No |

Hardware-in-the-loop tests cannot run in CI. They are run manually and their
logs committed as evidence.

## Regression policy

Any change touching the modem UART, GNSS power sequence, or socket layer must
re-run the **full** hardware regression baseline, because those are shared
subsystems and the failure modes are silent. A change to the web UI does not.
