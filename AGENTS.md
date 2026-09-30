# AGENTS.md — NomadLink operating manual

**Read this before you change anything in this repository.** It captures the
process this project follows so no step is skipped and no past mistake is
repeated. It is the single place that answers "what is the process, and what
must I never do again."

> If a fact here disagrees with a measurement, **the measurement wins** — but then
> fix this file in the same commit. A process document that drifts is worse than
> none, because it is trusted.

---

## 0. DO THIS NEXT

**One task. Flash the base firmware and confirm the SSID.**

1. `git push`
2. Open `https://ragavellur.github.io/nomadlink/project/index.html`
3. Plug the board in. Chrome, Edge or Opera. **Close every other serial monitor** —
   `screen`, `idf.py monitor`, Arduino IDE — they hold the port and the flash fails.
4. Press **Connect & Install**, pick the ESP32-S3 port, approve the prompt.
5. Wait for it to finish. The board reboots.
6. Join the WiFi network named **`NomadLink`** and confirm that is the name on
   your phone. Open the console at `http://192.168.4.1` and confirm it loads.

**Then report the result and stop.** Do not start a feature until this passes.

### How to answer

State only what you saw, for example: *"SSID is NomadLink, console at 192.168.4.1
loads"* — or paste the exact error. One sentence. No summary of the code, no
recap of what changed, no listing of gates. The user is tracking a flash, not
reading a report. If something fails, give the **exact error text** and stop.

**Never** claim this works before step 6 has been done by the user. It has not
been verified on hardware yet — see §10.

### The one rule behind this section

Changes in the repository do **not** reach the device or the web installer by
themselves. Two separate steps, both required:

| To change | You must run |
|---|---|
| Firmware behaviour (code) | `make stage-firmware` |
| The web installer page (JSON) | `make render` |

Skip either and the user flashes the old image while the source reads as changed.
That already happened once: the SSID stayed `ESP32_NAT_Router` after it had been
renamed, because the installer was serving a prebuilt binary that no source edit
could reach. If a user reports that a change "didn't take", check which of these
two steps was skipped before looking at the code. See L-16.

---

## 1. What this project is

NomadLink One: an ESP32-S3 + SIMCom A7670E travel gateway — dual-WAN failover,
camera streaming, GNSS location with cellular (LBS) fallback, direct MQTT
telemetry, SMS commands and telephony.

We are in the **foundation / de-risking stage**. Hardware capabilities are
physically verified; the product software has not been started. The dominant
risk in this project is **not functional bugs — it is false confidence**: a
capability reported as working, or as broken, when the evidence did not support
it. Every rule below exists to prevent that.

> **Where things stand:** the base firmware is in place and compile-gated, but it
> has **never been flashed from this repository**. Nothing else may start until the
> flash in §0 is confirmed by the user.

Canonical sources (read these, don't rely on memory):

| Topic | Source of truth |
|---|---|
| Product scope / requirements | `docs/product/product-spec.md`, `docs/product/requirements.json` |
| Task state | `docs/project/backlog.json` |
| Test results / evidence | `docs/project/test-results.json` |
| Defects | `docs/project/bugs.json` |
| Decisions | `docs/decisions/` (ADR markdown), `docs/project/decisions.json` |
| Architecture decisions rationale | `docs/decisions/README.md` |
| Test definition, DoD, hardware baseline | `docs/testing/test-strategy.md` |
| Hardware facts, gotchas, past mistakes | `LESSONS_LEARNED.md` |
| Sprint / release posture | `docs/project/sprint-board.json`, `docs/project/risks.json`, `docs/project/releases.json` |
| Activity log (append-only) | `docs/project/changelog.json` |

---

## 2. Truth model (do not fight this)

- **The repository is the source of truth** (ADR-001). Not a notes app, not chat.
- **JSON is authoritative; HTML is generated** (ADR-003). You edit
  `docs/project/*.json` / `docs/product/*.json`, then run `make render`. Never
  hand-edit generated HTML. After any JSON change, HTML and the JSON are
  **committed together** (rule 9 of the DoD).
- **Verified hardware facts override the PRD** (ADR-002). If the board disagrees
  with the spec, the board wins; record the discrepancy.
- **Decisions are immutable once Accepted** (`docs/decisions/README.md`).
  Superseding one creates a *new* ADR; never rewrite history. ADR-009
  supersedes ADR-005/007/008: the product is the vendored `esp32_nat_router`
  base on ESP-IDF, not the Arduino sketch.

---

## 3. The workflow (task state machine)

Every task moves through:

```
BACKLOG → READY → IN_PROGRESS → CODE_REVIEW → TESTING → VERIFICATION → DONE
```

- Pick work top-down: highest priority (P0) that is not blocked, respecting
  `dependencies` in `backlog.json`.
- **BLOCKED requires a recorded blocker reason** (validator rule 13). "Blocked"
  without a reason is a failure state.
- Move status forward only as the evidence for that gate exists. Do not
  pre-jump to DONE.
- Record every state transition with a timestamp (validator rule 23/24).

### Definition of Done (all must hold)

1. Code implemented and formatted.
2. Static checks pass where a toolchain exists.
3. Unit tests written and passing, for anything with logic.
4. Hardware regression re-run if the change could touch a proven capability.
5. Acceptance criteria individually satisfied, each with a recorded result.
6. Documentation updated (this file and `LESSONS_LEARNED.md` if a process fact changed).
7. Git commit created and the **real SHA recorded** in `backlog.json`.
8. Verification evidence recorded in **both** `backlog.json` and `test-results.json`.
9. Project JSON **and** regenerated HTML updated and committed together.
10. No known blocking defect.

**Code compiling is not verification. A device printing a success marker without
a captured log is not evidence.** A DONE task with empty `commit_sha`, empty
verification evidence, or `verification.status != VERIFIED` fails the validator
(rules 1–3).

---

## 4. Evidence discipline (the anti-false-confidence rules)

- **PASS** = a command was run and the expected output was observed.
- **FAIL** = a command was run and failed, exact form recorded.
- **NOT TESTED** = never attempted.

Never label NOT TESTED as FAIL or "unsupported". The distinction matters because
a FAIL invites a fix and a NOT TESTED invites a test. *(Two wrong "unsupported"
conclusions in this project came from exactly this.)*

A result line is a **claim**; the **raw bytes are the evidence**. Never record a
verdict without the raw response it came from (L-10). A verdict produced by a
parser you wrote — never run against this modem's actual output — is not
evidence. Evidence must be a captured serial log, an independent observer
("the user received the SMS", "the broker subscriber received the payload"), or
a measurement with the method stated.

Not acceptable as evidence: "it compiled", "the code looks right", "the test
passed" with no captured result, a marker printed unconditionally, or physically
impossible values. *(TASK-405's `MARK-CONNECTED` prints regardless of CONNECT
success; an old NMEA parser reported `satsUsed=99` — both are caught defects.)*

**A claim in a docstring is a claim, not a mechanism. A guard that has never
failed has not been tested** (L-08). When you touch a gate, run it against
input it is supposed to **reject** and confirm it fails.

---

## 5. Evidence taxonomy and hardware truth

### Hardware regression baseline (re-run after any change that could touch them)

| Capability | Sketch | Success signal | Preconditions |
|---|---|---|---|
| GNSS fix | `gnss_hold` | fix, quality 1, `sats_used > 0` | 4G patch antenna, open sky, uninterrupted power-on |
| Cellular LBS | `lbsmatrix` | `+CLBS: 0,<lat>,<lon>,<acc>` | antenna attached, `AT+SIMEI` populated |
| Data bearer | `lbsmatrix` | `+CGPADDR: 1,<non-empty ip>` | SIM attached/unlocked |
| Camera | `cam_diag` | non-zero luma variance | — |
| SD card | `sd_diag` | write/read/delete round trip | known-good FAT32 |
| SMS out | `sms_test` | `+CMGS: <ref>` + user confirms receipt | `AT+CNMI=2,1,0,1,0` |
| Voice out | `voice_sms` | `+VOICE CALL: BEGIN`, `+CLCC: ... 2` | — |
| Broker socket | `tracker` | `+CIPOPEN: 0,10` **and** an independent subscriber receives a payload | never assert on `MARK-CONNECTED` alone |

**Any change touching the modem UART, GNSS power sequence, or the socket layer
must re-run the full baseline** — those are shared subsystems with silent
failure modes. A web-UI change does not.

### Non-negotiable operating preconditions

- **Attach the 4G patch antenna.** Without it `+CSQ` drops 26→16 and LBS returns
  `+CLBS: 9`. Log `AT+CSQ` in the same run as any location query and record
  whether the antenna was attached — results across different antenna states are
  not comparable.
- **Populate `AT+SIMEI`** from `AT+CGSN`, or LBS returns `+CLBS: 12`.
- **Never power-cycle the GNSS engine mid-session** — it needs one uninterrupted
  power-on to download an almanac.
- Use **`AT+CIPOPEN`**, never `AT+CIPSTART` (returns `ERROR` on this firmware).
- Use **`AT+CLBS=1,1`**, never `AT+CLBS=1` — the CID is mandatory in practice.
- Run **`AT+CMD=?` before concluding a command is unsupported.** (`AT+CLBS=?` is
  what revealed the mandatory CID after four wrong attempts.)

### Verified pin map (ESP32-S3 rev2, 16 MB flash, 2 MB PSRAM, Arduino core 3.3.11)

| Signal | GPIO |
|---|---|
| Modem TX (module RX) / Modem RX (module TX) | 18 / 17 (`ss.begin(115200, SERIAL_8N1, 17, 18)`) |
| Modem module-enable rail (output HIGH) | 33 in firmware |
| PWRKEY | **not driven from firmware** — see below |
| I2C SDA / SCL (battery gauge MAX17048 @0x36) | 3 / 2 |
| Camera OV2640 D0-D7 / XCLK / PCLK / VSYNC / HREF / SDA / SCL | 7-14 / 34 / 37 / 36 / 35 / 15 / 16 |
| SD_MMC CLK / CMD / D0 | 5 / 4 / 6 |

**Correction (2026-09-30):** this table previously read "modem power rail 21 /
PWRKEY 42" and warned "never GPIO33". That was a pinout misreading, and it is
now wrong in the other direction. Measured and code-recorded: firmware drives
**GPIO33** as the module-enable rail, and **GPIO42 is not PWRKEY** — in the
schematic the PWRKEY net does not reach this board, so `powerOnModem()` asserts
the enable rail and waits rather than pulsing a key. PPP reaches the internet on
this wiring. Do not "correct" the firmware to 21/42 without a schematic and a
measurement; the earlier wrong claim about GPIO33 is what this corrects.

**A PWRKEY pulse is a power toggle on an off modem and an abort on a live one —
never send one speculatively** (L-11).

### Board / build facts

- FQBN partition scheme **must** be `app3M_fat9M_16MB`. A `default_8MB` build
  compiles perfectly and then **does not boot** — a compile gate cannot catch
  this, only the flash target can. `LESSONS_LEARNED.md` §Board has been corrected
  (2026-09-30); it previously still showed the stale `default_8MB`.
- Flash **app partition only** (`0x10000`). The bootloader/partition table on
  this board are known good; overwriting them bricks a booting board.
- USB CDC does **not** enumerate on this board. Use the UART path; do not build
  around USB CDC.
- SD: `SD_MMC.setPins(5,4,6); SD_MMC.begin("/sdcard", true, false, 20000, 5);`
  — the 4th arg is frequency in **kHz**. GPIO46 is **not** a card-detect line.

### AT command gotchas (one-liners; full table in `LESSONS_LEARNED.md`)

`AT+CNMP=38` (2G retired on Airtel, `2` fails). `AT+COPS?` unreliable — use
`+CGPADDR`. `AT+CGNSINF` does not exist. `AT+CMGL="SENT"` unsupported. `AT+CIPSEND`
`>` prompt and MQTT CONNACK bytes (`20 02 00 00`) are **not** newline-terminated —
read raw bytes, a line parser will hang. `AT+HTTPPOSTEX`/`HTTPREAD` unusable as a
publish path. `AT+CLBS` returns `OK` **before** the async `+CLBS:` URC.

### A silent UART is ambiguous

Before concluding the modem is dead: **try guarded `+++` first.** In PPP data
mode the A7670E discards AT *by design*, so "no AT reply" and "no power" look
identical. **A PWRKEY pulse is a power toggle on an off modem and an abort on a
live one — never send one speculatively** (L-11). When a probe is destructive,
never reuse its output as proof about the next action.

---

## 6. Commands (this is the gate loop)

```bash
make check      # validate + secrets + render + baseline build + product build. Must pass before ANY commit.
make validate   # tracker invariants (validator rules 1-13)
make secrets    # secrets scan; ARGS=--history to scan all commits
make render     # regenerate HTML from JSON after any JSON change
make baseline   # compile every hardware regression sketch
make product       # compile the product firmware (firmware/nat_router, ESP-IDF 5.5.x)
make stage-firmware  # build it AND stage it into docs/project/firmware/ for the web installer
make flash SKETCH=<name>   # flash ONE sketch's app partition (0x10000)
```

Flash port (CH343): `/dev/cu.wchusbserial58750034621`. Console/observe port
(CH9102): `/dev/cu.usbmodem58750034621`. Copy the secrets template first if a
sketch needs it: `cp firmware/baseline/tracker/secrets.h.example
firmware/baseline/tracker/secrets.h`.

> **The product firmware is `firmware/nat_router`, built with ESP-IDF 5.5.x** (ADR-009,
> 2026-10-01). It is vendored upstream `esp32_nat_router` 2.4.17 source, unmodified.
> `build-baseline.sh` still loops `firmware/baseline/*` only, so `make product`
> / `build-product.sh` is the only thing covering the product tree — it is part of
> `make check`.
>
> **The base must compile unmodified before you touch it.** A build that only
> succeeds after local edits is a fork, not a base. Provenance and licence are in
> `firmware/nat_router/README.md`.
>
> **It does not build on ESP-IDF 6.x** — `main` requires component `json`, which
> moved into the component manager, so you get `Failed to resolve component 'json'
> required by component 'main': unknown name`. That is a toolchain mismatch, not a
> code fault. Use the 5.5.x install (`~/esp/esp-idf-5.5.4/export.sh`, override with
> `NAT_ROUTER_IDF_EXPORT`). Do **not** "fix" it by editing the component list.
> The gate SKIPs cleanly when no 5.5.x install is found, so a missing toolchain
> cannot pass as a successful build — but a SKIP is not a PASS either.
>
> On this machine the IDF installer needs
> `SSL_CERT_FILE=/Library/Frameworks/Python.framework/Versions/3.13/lib/python3.13/site-packages/certifi/cacert.pem`,
> because the system python cannot verify TLS unaided. `build-product.sh` sets it
> if unset.

> **After any product firmware change that must reach users, run `make
> stage-firmware`.** The installer serves `docs/project/firmware/*.bin`, and those
> bytes are produced by that target. Renaming something in the source and committing
> changes nothing for anyone who flashes the page — that is exactly how the AP SSID
> stayed `ESP32_NAT_Router` while the repository said `NomadLink`. `make product`
> warns when the staged payload is older than the sources it came from. The product
> artifact is **`nomadlink.bin`**; the IDF project name in `CMakeLists.txt` is what
> `idf.py` names it after, so that rename is load-bearing. Brand strings live in
> `main/esp32_nat_router.c` (SSID), `include/router_config.h` (mDNS hostname) and
> `components/http_server/pages/page_index.h` (UI titles). The MQTT topic prefix
> stays `esp32_nat_router` on purpose — functional identifier, not branding.

> **The Arduino sketch at `firmware/nomadlink` is historical evidence only.** It is
> the prototype described in L-16 and is no longer the product. Do not extend it,
> and do not port its API shapes into the router base.


---

## 7. The rules referenced by number

These rule numbers are cited throughout the code and JSON. Machine-enforced
where noted; otherwise prose conventions you are expected to honour.

| Rule | Meaning | Enforced by |
|---|---|---|
| 13 | No unevidenced VERIFIED claim; BLOCKED needs a reason; PASS needs captured evidence | `validate.py` |
| 20 / 42 | JSON authoritative, HTML is generated — rerender after JSON change | `render.py` discipline |
| 23 / 24 | State transitions timestamped; changelog is **append-only**, never rewrite historical evidence | `validate.py`, `changelog.json` |
| 27 | **No secrets, no raw identifiers in git.** Anything committed or shared in plaintext must be **rotated**, not merely removed | `secrets_scan.py` (backstop only) |
| 37 | Do not call something a production release until readiness checks pass | `releases.json` |
| 44 | The 13 validator invariants (below) | `validate.py` |

**Validator invariants (Rule 44):** DONE needs VERIFIED + commit SHA + evidence;
non-BACKLOG needs acceptance criteria; orphan REQ/EPIC/FEAT/STORY/TASK/TEST/BUG/
RISK/ADR references fail; VERIFIED requirements need a verification note; a
missing requirement start is a WARN; invalid status/priority/severity fails.

> The full 1–44 rule catalogue is not written out in one file; the set above is
> the working set actually referenced. If you introduce a new rule, write it
> down here.

### Secrets (Rule 27) — beyond the scanner

No credential belongs in the repo; read them from gitignored `secrets.h`. The
scanner is a **backstop, not a substitute**. **IMEIs and phone numbers count**:
neither can be rotated after publication, which is exactly why they are easy to
under-rate. Redact device IMEIs as "this unit's IMEI (redacted; read from local
logs)". If a credential or live identifier was committed, it must be **rotated
at the source** — and because git history is append-only, an unpushed leak is
rewritten (squash/reset + recommit), not merely deleted from the working tree.
Hand-editing fixes only the instances you happen to see; prefer adding the
pattern to `secrets_scan.py`.

---

## 8. Lessons-learned discipline

`LESSONS_LEARNED.md` is the memory that prevents repeat mistakes. Maintain it.

- **Format:** `## L-NN — short lesson title (YYYY-MM-DD)`, appended at the end,
  in the same voice as the existing entries: what happened, the wrong turn taken,
  the general rule, and what now catches it.
- **When to add one:** any time you were wrong, nearly wrong, or a verdict
  disagreed with reality — including "the control passed so the regression isn't
  mine." Record the mistake you actually made, not a sanitised version.
- **Cross-link** new lessons to the `MISTAKES MADE (do not repeat)` and
  `THE THREE CRITICAL THINGS` sections so the whole history stays coherent.
- Current lessons run `L-08` … `L-16` (see the file). This is the fastest
  defence against the traps below.

### The traps, consolidated (do not repeat)

- Never trust a verdict from a parser you wrote without the raw modem response.
- Never declare a command unsupported without `AT+CMD=?`, and never call "no AT
  reply" a dead module until you have tried guarded `+++`.
- Never compare results across differing hardware states (antenna on/off, CSQ
  16 vs 26) and call the difference firmware.
- Never write an unverified negative into the docs — it compounds into a false
  defect that stops anyone re-testing (this is how "LBS unsupported" and "socket
  path broken" got written up as settled).
- Never hand-roll NMEA field indexing (GGA lat/lon are two fields each); use
  TinyGPS++.
- **Check whether a working implementation already exists before building one**
  (L-16). A reference implementation means *use it*, not *match its behaviour*.
  A firmware the user has already flashed and called working is the base.
- **`git check-ignore` before you commit a binary.** `*.bin` is ignored
  repo-wide, which silently excluded the web installer's firmware parts; a
  correct local page is not proof the published page works.
- **A compile gate must check the exit code, and must be proven to fail.**
  One reported `ok` while building nothing, because it grepped the log and
  ran `idf.py` from the wrong directory (L-16).
- **A compile error names a symbol, not a library.** Before writing a dependency
  incompatibility into a doc or an ADR, grep the library headers *and* the core
  headers to see which one owns the missing symbol. Four times on 2026-09-30 a
  "problem" was a function that never existed in core 3.3.11
  (`ESP.getChipId`, `softAPgetStationInfo`, `getMinEverFreeHeap`, and
  `esp_wifi_ap_get_sta_list_with_ip` needing its own include). One of those
  would have permanently downgraded the web stack (L-14).
- **The web console must never touch the UART, and must never show an
  unmeasured number.** Modem telemetry is sampled in the one safe window before
  the PPP dial, then cached, and the UI shows the *age* of the sample. A section
  with no data says NOT INTEGRATED and names its owning TASK. A client with no
  lease shows `no address yet` — never a borrowed address. See ADR-007.
- **When something that worked stops working, reproduce the old flow
  byte-for-byte on the committed control before touching firmware** (L-13). A
  true cold power-cycle (long PWRKEY off/on) is not an ESP32 reset. Prove the
  data plane separately from the application.
- If the unchanged control now fails, the regression is external (service /
  physical setup), not yours — do not go firmware-hunting.
- When two independent subsystems fail in the same window, suspect a shared
  physical cause (a moved portable unit, a carrier outage) and check the setup
  before the build.

---

## 9. Git & commit discipline

- `make check` passes **before any commit**. This is non-negotiable.
- One logical change per commit; write the *why* in the message.
- Record the real commit SHA in `backlog.json` after committing; don't guess it.
- Never rewrite **pushed** history. Rewriting an *unpushed* local commit to purge
  a secret is correct and expected (see §7).
- Append to `changelog.json`; never edit a prior entry (Rule 24).

---

## 10. Current state (2026-10-01) — read before planning

- **The product base is `firmware/nat_router`** (vendored `esp32_nat_router`
  2.4.17, ESP-IDF 5.5.x) per ADR-009. It compiles unmodified and is compile-gated.
  A browser web installer for the base image is generated at
  `docs/project/index.html` from `docs/project/firmware.json`, serving
  `docs/project/firmware/*.bin` via `manifest_nomadlink_esp32s3.json`.
  The page is labelled **base-only** — the A7670E 4G, GNSS, camera and SD
  features are **not** in that image. Actual flashing is **NOT TESTED**; Web
  Serial needs the user's click and a connected board.
- **RISK-011 is open (S2):** upstream ships **no LICENSE file** and GitHub
  reports `license=null`. Redistribution rights are unstated. Nothing may be
  called a production release until this is settled (Rule 37).
- Two artifacts exist and are **not** interchangeable: the served binary is
  upstream's prebuilt (1,370,000 B); the local rebuild from vendored source is
  1,374,576 B. Toolchain drift, same base. The local build replaces the served
  one when features land.
- **No NomadLink feature has been added to the base yet.** That is the next task,
  and the user asked to confirm the base first.

## 10b. Current state (2026-09-30) — read before planning

- **Sprint-001** (Foundation, safety, de-risking; ends 2026-10-10) is
  `IN_PROGRESS`. Open exit criteria: ADR-005 framework decision still only
  *Proposed* (its PPP precondition — RISK-002 — is now met, so it can move to
  Accepted); RISK-003 audio hardware population is **blocked on the user's
  physical board inspection** (TASK-021); RISK-001 GPS cold-start benchmark
  unstarted (TASK-022).
- **Two location regressions are open and scheduled for a re-verify on
  2026-10-01** — do not lose this date:
  - **BUG-009 / LBS / TEST-302 / REQ-008.** `AT+CLBS=1,1` gave `+CLBS: 0` on
    09-29 and `+CLBS: 10` (close network error) on 09-30 for every mode. The
    unmodified `lbsmatrix` control and a true cold power-cycle both failed, and
    the data plane is healthy — the failing boundary is the carrier/SIMCom LBS
    service (no `AT+CLBSCFG` on A76XX). **Re-run the unmodified control; if it
    still returns 10, escalate to the carrier, change no firmware.**
  - **BUG-010 / GNSS / TEST-301 / REQ-007.** Fixed at 17:46 local, then
    `satsUsed=0` / `fix=no` for 500 s across four runs while NMEA kept streaming.
    Signature = RF / sky-view / antenna, not firmware. **Physically inspect and
    reseat the patch antenna, restore the clear-sky placement, re-run the
    unmodified `gnss_hold`.**
- **Next P0 in the queue:** **TASK-002** (baseline regression sketches with
  uniform markers + documented flash/observe procedure; test TEST-001). It is
  READY, depends only on DONE work, and gates TASK-303 (LBS fallback) and
  TASK-302 (position service). Standardizing the sketches is what makes the
  2026-10-01 re-verifies repeatable — the markers are currently inconsistent
  across `gnss_hold`, `lbsmatrix`, `lbs_debug`, `position_service`.
- **Needs the user physically** (cannot be closed in software): a real client
  lease on the SoftAP (TASK-101, IN_PROGRESS) and the audio board inspection
  (TASK-021).

---

## 11. When you are unsure

- Prefer a measurement to an argument. If you cannot measure it, record it as
  **NOT TESTED**, not as a conclusion.
- If a recorded fact looks wrong, **re-run the committed control** before
  trusting your newest experiment.
- Ask before changing hardware state (power-cycling GNSS, moving the unit) or
  anything that a re-verify date depends on.
- When you fix a process gap, update this file and `LESSONS_LEARNED.md` in the
  same commit as the fix.