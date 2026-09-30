# Lessons Learned — ESP32-S3 + A7670E (Waveshare V1)

Working reference for this board. Read before touching the modem or GNSS again.

## THE THREE CRITICAL THINGS

### 1. PWRKEY power-on pulse is mandatory — GPIO42

The A7670E needs a real power-on sequence. Merely holding the module out of reset
is NOT enough for the GNSS engine.

```cpp
pinMode(MODEM_PWR_PIN, OUTPUT);
digitalWrite(MODEM_PWR_PIN, HIGH);       // GPIO21 = peripheral power rail

pinMode(MODEM_PWRKEY, OUTPUT);
digitalWrite(MODEM_PWRKEY, HIGH);
delay(100);
digitalWrite(MODEM_PWRKEY, LOW);
delay(1200);                             // 1.2s low pulse
digitalWrite(MODEM_PWRKEY, HIGH);
delay(5000);                             // 5s for module to settle
```

**What happens if you skip this:** the module still answers `AT`, still streams
NMEA, and still emits *checksum-valid* sentences — so every naive test says the
GNSS is fine. But it never cold-started, so it never downloads a fresh almanac.
It recites the stale one from flash.

The tell-tale signature I wasted hours on:
- C/N0 field **empty** for every satellite
- azimuth field **empty** for every satellite
- elevation field **populated** and drifting slowly (e.g. 20° -> 37° over minutes)
- GGA quality 0, 0 sats used, HDOP 99.99, RMC status `V`

Elevation-only data is predicted position from a stored almanac, NOT tracking.
I read this as "no sky view indoors" and told the user to move the board
outdoors. Wrong diagnosis — the board may well have been outdoors, or indoors
irrelevant. The module had never been power-started.

### 2. NMEA port must be `CGNSSPORTSWITCH=1,1` — not `0,1`

```cpp
SentMessage("AT+CGNSSPORTSWITCH=1,1");   // hardware UART matrix port
```

`1,1` = the physical UART port matrix, which is where `ss.begin(..., 17, 18)` is
actually listening.

I used `0,1`, which routes NMEA to the **USB CDC** path. On this board A7670E
USB CDC never enumerates, so that is the one path guaranteed to be dead. NMEA
still leaked onto the UART anyway, which is why it *looked* like it worked and
sent me down the wrong path again.

Full GNSS bring-up sequence:

```cpp
SentMessage("AT+CGNSSPWR=1");
delay(1000);
SentMessage("AT+CGNSSPORTSWITCH=1,1");   // <-- must be 1,1
delay(500);
SentMessage("AT+CGNSSTST=1");            // start the NMEA pump
delay(500);
```

**Update 2026-09-30 (unit rev A7670M7_V1.11.1):** on this board/firmware the
observed behaviour is that `AT+CGNSSPORTSWITCH=1,1` returns `ERROR` and no NMEA
reaches the UART, while `AT+CGNSSPORTSWITCH=0,1` returns `OK` and streams NMEA,
producing a real lock (t+130s, 4 sats, hdop 7.3). `gnss_hold.ino` now uses
`0,1`. If `1,1` is again seen working on another unit, capture `AT+CGNSSPWR?` /
firmware rev at boot before trusting either form.

### 3. Cellular LBS fallback — `AT+CLBS=1,1`, and the CID is NOT optional

I spent a long time telling the user that LBS did not work on this module. **It
does.** It returned `+CLBS: 0,18.481703,73.897415,550` on the same SIM and firmware
I had already declared incapable. Two things had to be true at the same time, and
I only ever had one of them:

**(a) The CID argument is mandatory in practice.** `AT+CLBS=?` gives the real
signature:

```
+CLBS: (1,2,3,4,9),(1-15),(-180.000000-180.000000),(-90.000000-90.000000),(0,1)
     ^modes          ^CID    ^lon range                  ^lat range      ^extra
```

`AT+CLBS=1` (mode only) defaults the CID to a context the LBS engine cannot
use and returns `+CLBS: 9`. **`AT+CLBS=1,1` works.** My earlier "LBS is broken"
conclusion came from never passing a CID.

**(b) The patch antenna has to be connected.** Before the antenna, `+CSQ: 16`;
after, `+CSQ: 26,99`. ~26 dB gain. Weak signal alone will make the location
server lookup fail, and that failure is indistinguishable from a wrong-argument
failure unless you check signal strength in the same log.

Working sequence:

```cpp
SentMessage("AT+CGACT=1,1");                 // activate the data bearer
delay(6000);
SentMessage("AT+CGPADDR=1");                 // verify, expect a real IP
// populate the IMEI the LBS engine authenticates with
SentMessage("AT+SIMEI=<IMEI>");               // value = result of AT+CGSN; the real IMEI is redacted from this repo
SentMessage("AT+SIMEI?");                    // confirm it stuck
String r = sendAT("AT+CLBS=1,1", 150000);    // 150s, not 10s
// +CLBS: 0,18.481703,73.897415,550
```

Field order is **lon, lat** — longitude first. The reference snippet in the
module docs parses it correctly; a hand-rolled parser swapped them.

| Variant | Result | Use |
|---|---|---|
| `AT+CLBS=1,1` | `+CLBS: 0,lon,lat,acc` | **This one.** Numeric fix. |
| `AT+CLBS=2,1` | `+CLBS: 0,` + hex garbage | Formatted text, unsupported in this region. |
| `AT+CLBS=9,1` | `+CLBS: 0` | Status/query mode, returns no coordinates. |
| `AT+CLBS=1` (no CID) | `+CLBS: 9` | Wrong — default CID is unusable. |
| `AT+CLBS=1,1` (before SIMEI set) | `+CLBS: 12` | IMEI not populated. |

`SIMEI` is writable and survives in flash: `AT+SIMEI?` returned
`+SIMEI: <IMEI>` after I set it, and `CLBS` still failed until the
antenna was attached — so IMEI alone is necessary, not sufficient.

## HOW I GOT THIS WRONG — the actual process failure

Recording this because the method was wrong, not just the syntax.

1. **I declared a feature absent from two failing invocations.** I had tried
   `CLBS?`, `CLBS=1,1`, and `CENG?` — all failed — and generalised to "this
   module has no cell-tower positioning." I never ran `AT+CLBS=?` to discover
   the real parameter list. **The capability query was available the entire
   time and costs one AT command.**
2. **I told the user it was impossible when I had only tested two forms.** The
   user kept saying it worked before, with the same SIM. That was evidence
   contradicting me and I treated it as noise instead of as a signal that my
   test was wrong. When the user is confident about prior hardware behaviour,
   the default hypothesis is **my test, not their memory**.
3. **I asserted a "not supported on this firmware" conclusion and stopped
   investigating**, then repeated it in a status table to the user. Writing a
   claim into a table makes it look verified to future-me, so a guess
   compounded into several.
4. **I floated a context-mismatch theory, then another (SIMEI), without testing
   the one the capability query pointed at.** Each theory felt plausible and I
   chased it while the cheap, decisive test sat unrun.
5. **I kept an unverified negative in the AT command table** (`CLBS` marked
   non-working) which would have stopped anyone re-testing it later.

The rules I should have followed:

- **Never record "unsupported" from failed invocations.** Record the exact
  invocations tried, then either run the capability query or leave it open.
- **`AT+CMD=?` first, always.** It is the cheapest possible way to find out what
  parameters exist. I skipped it repeatedly.
- **Enumerate the parameter space before blaming hardware.** One `<mode>,<cid>`
  pair differed from what I'd been sending.
- **Control every variable before concluding.** The antenna was connected and
  unplugged across my test runs. I compared results across different hardware
  states and treated the difference as a firmware fact.
- **"It never worked here" is not the same as "it does not work."** I had no
  record of a prior success on this board, so I had nothing to contradict — but
  I also had nothing proving failure, only failed tests.

Cost of this: the user was told a working feature was missing, twice, and had to
push back three times before I ran the capability query that answered it in one
line.

## PIN MAP (verified)
| Signal | GPIO | Notes |
|---|---|---|
| Modem TX (module RX) | 18 | `ss.begin(115200, SERIAL_8N1, 17, 18)` |
| Modem RX (module TX) | 17 | |
| Modem power rail | 21 | output HIGH, keep HIGH |
| PWRKEY | 42 | 1.2s LOW pulse to start the radio core |
| I2C SDA / SCL | 3 / 2 | `Wire.begin(3, 2)` |
| Battery gauge MAX17048 | 0x36 | I2C addr |

I previously drove **GPIO33** for modem control because I misread the vendor
pinout. GPIO33 is not the modem control line on this board — 21 and 42 are.
Holding GPIO33 HIGH did let AT commands through, which is why it *seemed* to
work, but the module was never properly started.

## 4G PATCH ANTENNA — must be connected

The cellular patch antenna changed LBS from failing to working, and I did not
control for it while testing. Signal strength with and without:

| State | `AT+CSQ` | LBS result |
|---|---|---|
| No antenna | `+CSQ: 16,99` | `+CLBS: 9` |
| Patch antenna attached | `+CSQ: 26,99` | `+CLBS: 0,18.481703,73.897415,550` |

~26 dB gain. A weak-signal lookup failure is indistinguishable from a
wrong-argument failure unless you log `AT+CSQ` in the same run. **Always log
signal strength alongside any location query, and record whether the antenna was
attached for that specific run.** Because I did not, I compared results across two
different hardware states and attributed the difference to firmware.

## AT COMMAND NOTES

| Command | Correct | Wrong / gotcha |
|---|---|---|
| `AT+CPIN?` | `+CPIN: READY` | |
| `AT+CIMI` | SIM IMSI | |
| `AT+CNMP` | `38` = LTE only | `2` = GSM/2G. Airtel India has retired 2G -> `+CME ERROR: no network service`. There is no 2G fallback on this SIM. |
| `AT+COPS?` | unreliable, ignore | Returns `+COPS: 0` even when registered. I used it as a network gate and it stalled boot. Use `+CGPADDR` instead. |
| `AT+CGPADDR=1` | **use this to verify data layer** | Best bounded network check. Look for `+CGPADDR: 1,"<ip>"` with a non-empty IP and no `""`. |
| `AT+CEREG?` / `AT+CREG?` | LTE vs GSM registration | `+CREG: 0,6` = not registered for circuit-switched. SMS/voice need GSM registration. |
| `AT+CSCA?` | `+CSCA: "<smsc number redacted>",145` | SMSC resolved from the SIM. |
| `AT+CMGS="<num>"` | then body, then `0x1A` | Wait for the `>` prompt first. Returns `+CMGS: <ref>`. |
| `AT+CMGL="SENT"` | **not supported** | `+CMS ERROR: Operation not allowed`. Can't read the sent box back. |
| `AT+CNMI` | `2,1,0,1,0` to get delivery reports | `2,1` leaves status reports OFF -> `delivery reports seen: 0` |
| `ATD<num>;` | returns `+VOICE CALL: BEGIN` | Poll `AT+CLCC?`: state `2` = active, `4` = held. Hang up with `AT+ATH`. |
| `AT+CGPSINFO` | valid | |
| `AT+CGNSSINFO` | valid | |
| `AT+CGNSINF` | **DOES NOT EXIST** | I invented it and treated `+CME ERROR` as a hardware fault. |
| `AT+CLBS=1,1` | **the working LBS form** | CID is mandatory in practice. `AT+CLBS=1` alone returns `+CLBS: 9`. Run `AT+CLBS=?` before assuming anything. |
| `AT+SIMEI=<imei>` | set from `AT+CGSN` result | LBS engine authenticates with the module's own IMEI. Unpopulated -> `+CLBS: 12`. |
| `AT+CIPOPEN` | **use this, not `CIPSTART`** | `AT+CIPSTART` returns `ERROR` on this module/firmware. `AT+CIPOPEN=0,"TCP","host",port` returns `+CIPOPEN: 0,10` and the socket connects. `CIPOPEN` is what the TinyGSM A7672x driver uses. I declared the socket path broken from `CIPSTART` failures alone. |
| `AT+CIPSEND` | prompt `>` may have **no trailing newline** | A line-based reader that blocks on `\n` will miss the prompt and time out. Read the `>` as a raw byte. |
| MQTT over this socket | `+CIPSEND: 0,<len>` then raw binary | Broker replies (e.g. CONNACK `20 02 00 00`) are **not newline-terminated**. A line parser cannot read them. |
| `AT+HTTPPOSTEX` / `AT+HTTPREAD` / `AT+HTTPSTATUS` | **ERROR on this module** | `AT+HTTPACTION=0` does initiate a GET and reports `+HTTPACTION: 0,714,0`, but you cannot read status or body, and cannot POST. Not usable as a publish path. |

## VERIFIED WORKING CONFIGURATION

### SD card
```cpp
SD_MMC.setPins(5, 4, 6);                            // V1: CLK=5, CMD=4, D0=6
SD_MMC.begin("/sdcard", true, false, 20000, 5);     // freq is kHz!
```
- 4th arg is **frequency in kHz** (20000 = 20 MHz), not block size.
- `format_if_mount_failed = true` to format an unformatted card.
- **GPIO46 is NOT a usable card-detect line.** It reads HIGH with a working card
  present. Do not gate `SD.begin()` on it — that produced a false FAIL on good
  hardware.
- Confirmed: 3,984,588,800-byte FAT32 card, write/read/delete round-trip OK.

### Camera (OV2640, V1 pinout)
D0-D7 = GPIO 7-14, XCLK=34, PCLK=37, VSYNC=36, HREF=35, SDA=15, SCL=16,
WS2812B = GPIO38.
- MID `0x7fa2`, PID `0x0026`, VER `0x0042`, SCCB addr `0x30`.
- Confirmed 320x240 RGB565 in PSRAM. Mean luma 35.0, stddev 15.4,
  block stddev 9.2. **The non-zero variance is what proves a live sensor** —
  a dead/failed init also gives a flat fill, so check variance, not just "it
  compiled".
- Reading reg 0x0A/0x0B returns 0 under 16-bit addressing. Not a fault.

### Board
- ESP32-S3 rev2, 16 MB flash, 2 MB PSRAM.
- Arduino core **3.3.11**, FQBN:
  `esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=dio,FlashSize=16M,PartitionScheme=default_8MB,DebugLevel=none,PSRAM=enabled,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default`

## DEBUGGING RULES I BROKE

### Use TinyGPS++, not hand-rolled field parsing
My hand-written NMEA parser was wrong twice. GGA has a gotcha: **latitude and
longitude each occupy two fields** (value + N/S/E/W hemisphere), so field indices
shift by 4, not 2.

```cpp
// GGA:  0 ID, 1 UTC, 2 lat, 3 NS, 4 lon, 5 EW, 6 quality, 7 numSV, 8 HDOP, 9 alt
// GSV:  0 ID, 1 numMsg, 2 msgNum, 3 totalSV, then (prn, C/N0, az, el) step 4
```

My errors, in order:
1. Assumed `satsUsed` = field 5. Wrong — GGA lat/lon are 2 fields each.
2. Constellation test `strncmp(line+3, "GPS", 3)` reads `GSV`, not `GPS`. The
   ID starts at `line+1`.
3. Produced `satsUsed=99`, `hdop=0.00`, `utc=$GNGGA` — garbage — and then
   reported it as a result. **A diagnostic reporting obviously impossible
   values should be treated as a broken diagnostic, not as a device fault.**

`TinyGPSPlus` handles this correctly and gives `isValid()` flags. Use it.

Note that LBS is the opposite case: there is no library, the value is a single
`+CLBS:` line, and it is **longitude first, latitude second**. That inversion is
easy to get wrong when the two sources are compared side by side.

### Run the capability query before declaring a feature absent
`AT+<CMD>=?` costs one command and returns the accepted parameter list. I never
ran it for `CLBS`, and its output is the single line that explained four
consecutive failed attempts. Any time a command fails, the next action is to
read its parameter spec, not to form an opinion about the hardware.

### Always echo raw bytes to the host
A 12-minute watch counted 5760 NMEA sentences on-device but never forwarded
them to the USB console, so the log held only my own summary lines. When the
summary looked wrong I had no ground truth to re-analyse. **Forward every
sentence to Serial0** so failures are diagnosable after the fact.

### Verify checksums before trusting a stream
```python
def csum(s):
    body, given = s[1:].rsplit('*', 1)
    c = 0
    for ch in body: c ^= ord(ch)
    return "%02X" % c, given.upper()
```
This correctly separated genuine NMEA from noise, and is how I confirmed the
sentences were real-but-empty rather than corrupt.

### Don't name locals the same as SDK macros
`PIN_RX` / `PIN_TX` collided with `soc/reg_base.h` and broke the build with a
misleading error pointing into the ESP-IDF headers. Renamed to `GP_RX`/`GP_TX`.
**Beware: any `static const int` colliding with an IDF macro can surface as a
bizarre error far away, inside a core header.**

## MISTAKES MADE (do not repeat)

1. **Skipped the PWRKEY (GPIO42) power-on pulse.** Concluded "no satellite
   signal outdoors is a hardware/antenna problem" when the real cause was the
   module had never been started. Sent the user outdoors for nothing.
2. **Used `CGNSSPORTSWITCH=0,1` instead of `1,1`.** Routed NMEA to the dead USB
   CDC path instead of the live UART.
3. **Used GPIO33 for modem control** because I misread the vendor pinout.
   Correct pins are 21 (power rail) and 42 (PWRKEY).
4. **Invented `AT+CGNSINF`.** It does not exist. `+CME ERROR` from a
   non-existent command looks identical to a hardware failure.
5. **Hand-rolled NMEA parsing, wrong twice**, then reported the garbage output
   as device state. `satsUsed=99` was a parser bug, not a sensor fault.
6. **Gated on `AT+COPS?`**, which is unreliable on this module. Used
   `+CGPADDR` instead.
7. **Gated `SD.begin()` on GPIO46**, which is not a real card-detect line.
   Caused a false FAIL on working hardware.
8. **Did not forward raw NMEA to the host**, leaving no ground truth to
   re-analyse when results looked wrong.
9. **Ran `AT+CNMP=2` (2G) expecting a fallback.** Airtel retired 2G. There is no
   2G fallback on this SIM; CNMP must stay 38.
10. **Set `AT+CNMI=2,1` expecting delivery reports**, but that leaves status
    reports off. Needed `2,1,0,1,0`.
11. **`PIN_RX`/`PIN_TX` collided with IDF macros**, producing a nonsense error
    inside `soc/reg_base.h`.
12. **Asserted a "no carrier" conclusion from `AT+COPS?`** and told the user the
    board was faulty, when the module was fine and the command was the problem.
13. **Declared cellular LBS unsupported from failed invocations alone.** Never ran
    `AT+CLBS=?`, which shows the real signature and that the CID is required. I
    told the user twice that a working feature was missing, and told them so
    after they had already said it worked. Overrode the user's direct
    observation of prior hardware behaviour in favour of my own failed test.
14. **Blamed the wrong subsystem for a socket problem.** `CIPSTART` fails on this
    module; `CIPOPEN` works. I reported the whole cellular path as unusable
    because I'd only tried the command the TinyGSM driver doesn't use.
15. **Compared results across differing hardware states.** The patch antenna was
    connected for part of the LBS testing and not the rest. `+CSQ: 16` -> `26`.
    I treated results spanning different antenna states as firmware facts.
16. **Wrote unverified negatives into the docs**, which compounded a guess into
    several: the `CLBS` row marked non-working would stop anyone re-testing it.

## VERIFIED TEST RESULTS

| Item | Result |
|---|---|
| SD card | PASS — 3.98 GB, read/write/delete OK |
| Camera | PASS — OV2640 confirmed, luma stddev 15.4 |
| GNSS fix | PASS — `18.480776, 73.897998`, 69 s cold start, 17 GPS + 10 GLONASS |
| 4G / LTE data | PASS — `+CEREG: 0,1`, `+CGPADDR: 1,100.90.93.199`, `+CSQ: 26,99` |
| **Cellular LBS** | **PASS — `AT+CLBS=1,1` -> `+CLBS: 0,18.481703,73.897415,550`** |
| Socket to broker | PASS — `AT+CIPOPEN` -> `+CIPOPEN: 0,10` (note: NOT `CIPSTART`) |
| MQTT path (macOS) | PASS — HiveMQ Cloud WSS 8884, authenticated QoS 1 round trip |
| 2G fallback | N/A — retired on Airtel |
| Voice call | PASS — `+CLCC: 1,0,2,...` state 2 = active, user heard audio |
| SMS | PASS — `+CMGS: 2`, `+CMGS: 3`, user received both |

**Two independent sources agree on position**, which is what makes the LBS result
credible rather than a plausible-looking number: GPS `18.480776, 73.897998` vs
LBS `18.481703, 73.897415`, separated by roughly 100 m and well inside the
550 m accuracy the module itself reported.

## CAVEAT ON THE STATUS TABLES I GIVE THE USER

Two conclusions in tables I previously presented were wrong and had no test
behind them: "cellular LBS unsupported" and "the cellular socket path is broken."
Both were written up as settled. When reporting status, distinguish:

- **PASS** — a command was run and the expected output was observed.
- **FAIL** — a command was run and failed, with the exact form recorded.
- **NOT TESTED** — never attempted. This is what both of the above really were,
  and I labelled them FAIL / unsupported. The distinction matters because a FAIL
  invites a fix and a NOT TESTED invites a test.

## USB CDC (known limitation)
A7670E USB CDC does not enumerate on either the Mac or the ESP32-S3 USB host,
despite the module answering over UART. Don't build around USB CDC on this
board — use the UART path. (The vendor PPP firmware flashes and boots but hangs
at `USB_HCDC: Waiting CDC Device Connection`.)

## L-08 — A guard that has never failed has not been tested (2026-09-29)

Two of the three project gates had been marked "working" without evidence that
they ever *rejected* anything.

`secrets_scan.py` claimed in its own docstring that gitignored files were not
scanned. They were: it walked the filesystem with `rglob` and never consulted
git. It only appeared to pass because `secrets.h` did not exist yet. The first
real run — after `build-baseline.sh` created one — failed immediately, and the
docstring's claim was exposed as false.

It also skipped every file whose extension was not in an allow-list, so
`secrets.h.example` was never scanned. That is the exact file where a real
password ends up when someone pastes one in and forgets to revert it. The first
attempt at fixing it passed the tracked-leak case and silently missed that one.

Both bugs were found by seeding deliberate violations and checking the outcome
per case, not by re-reading the code. The lesson generalises past this project:

- A gate must be run against input it is supposed to reject. Green on the happy
  path is not evidence.
- A claim in a docstring is a claim, not a mechanism. Verify it or delete it.
- Prefer allow-lists that are explicit about the dangerous case. An allow-list
  of extensions will always miss the next file type; ask "where would a secret
  plausibly land?" and enumerate that.

The validator was tested the same way (TEST-002) and behaved correctly, which is
the only reason to believe it.

## L-09 — Redacting an identifier is not the same as protecting it (2026-09-29)

The first commit went out with a real IMEI and two real phone numbers in it. A
manual `grep` for digit runs had missed one of them, because it had a `+91`
prefix and sat in a table cell.

The correction that mattered was adding IMEI and phone-number patterns to
`secrets_scan.py`, not the hand-editing. Hand-editing fixes the three instances
I happened to find. A pattern fixes the next one, and every future one.

Neither of these is a credential: neither can authenticate, and neither can be
rotated after publication. That is exactly why they are easy to under-rate. An
IMEI identifies a physical device permanently. Treat "it is not a secret" as a
reason to check, not a reason to skip.

Also noted: the scanner began flagging its own docstring examples, and its first
country-code stripper ate the leading digits of the numbers it was meant to test
(`^\+?\d{0,2}` turned 9876543210 into 76543210). A heuristic that mangles its own
input will quietly stop detecting anything. Synthetic-number detection has to be
unit-tested against real inputs, not reasoned about.

And the redaction itself broke two sketches: a note inserted between a `#include`
and a declaration landed outside the comment block, and the leading `*` parsed
as code. The baseline build gate caught it immediately, which is the only reason
it was not committed. This is what the gate is for.

## L-10 — A PASS/FAIL line is a claim; the raw bytes are the evidence (2026-09-29)

`firmware/baseline/ppp_probe` printed `RESULT S3_DATA_BEARER FAIL` directly
above this raw response:

```
> AT+CGPADDR=1
< +CGPADDR: 1,100.77.145.7 | OK
```

The data bearer had **passed**. My verdict parser only matched the quoted form
`+CGPADDR: 1,"..."`, and this firmware returns the unquoted form. Had the summary
line been all I captured, I would have recorded "Airtel does not provide data" —
the exact opposite of the truth — and built on it.

This is the same failure mode as claiming a capability is unsupported: a verdict
produced by a parser I wrote, which I had never run against this modem's actual
output. The rule is now: **a result line never gets recorded without the raw
response it was derived from.** Both are in TEST-020's evidence field so the
verdict can be re-checked later.

## L-11 — PWRKEY is not a reset, and AT silence does not mean the modem is off

`ATD*99#` returned `CONNECT 115200`, the modem entered PPP data mode, and it
then stopped answering AT. My recovery probe escalated hard and got nothing:

- 5 baud rates (115200 / 9600 / 57600 / 38400 / 230400)
- PWRKEY held 1200 ms, 2500 ms, 4000 ms
- an 8-second deep power-down via GPIO21
- **zero unsolicited bytes** in every case

I concluded the module had latched off, wrote that up as a severity-1 bug
(BUG-005), and had the user unplug the USB cable. **That diagnosis was wrong.**
The module was never off. In PPP data mode the A7670E discards AT commands *by
design*, so the silence was the expected state. A guarded `+++` escape restored
AT instantly, and once PPP was actually negotiated the same module held a stable
session without any power cycle at all.

The rule that would have saved all of this:

> **A silent UART is ambiguous. Prove which state you are in before you conclude
> the hardware is dead.** In data mode AT is discarded, so "no reply to AT" and
> "no power" look identical from the outside.

1. **A PWRKEY pulse is not a reset.** It is a power *toggle* on an off modem and
   a hang-up/abort on a live one, so a blind pulse can make things worse. My
   first `ppp_probe` pulsed unconditionally. Never send one speculatively.
2. **Try `+++` before concluding anything.** Guarded by silence before and
   after, it is the cheapest test that distinguishes data mode from dead.
3. **Escalating probes against an unverified hypothesis produce confident
   nonsense.** I ran a 2x2 GPIO21 x PWRKEY matrix and reported it as proof that
   polarity was not inverted. It proved nothing — the modem was in data mode and
   could not have answered either way.

## L-12 — lwIP PPP on this core needs three things the WiFi stack normally does

Getting PPP to negotiate on the A7670E took three independent fixes, each of
which produced *exactly the same symptom* — a completely silent link. That is
why it was so hard to see.

1. **Wait for the modem to enter data mode.** `CONNECT` is printed before the
   module is listening. lwIP's first Configure-Request went out microseconds
   later and was swallowed; the module then ignored the retransmissions too.
   The tell: a hand-built request with identical framing was answered at T+15s,
   while the byte-identical frame from lwIP at T+0ms was ignored. Content was
   never the problem — timing was. Fix: 2000 ms settle after `CONNECT`.
2. **Call `tcpip_init()` yourself.** `pppos_input_tcpip()` posts to the tcpip
   mbox, which is normally created by the WiFi library. A sketch that never
   starts WiFi never creates it, and the first inbound byte dies on
   `assert failed: tcpip_inpkt ... (Invalid mbox)`. Being in the *receive* path
   makes it look like the link came up and then vanished.
3. **Hold the core lock.** This core sets `CONFIG_LWIP_TCPIP_CORE_LOCKING=1`, so
   `pppos_create()` (which calls `netif_add()`) and `ppp_connect()` require
   `LOCK_TCPIP_CORE()` / `UNLOCK_TCPIP_CORE()` around them.

General lesson: **the Arduino lwIP build is hosted and WiFi-shaped.** Core
init, the mbox, and the locking discipline are all provided by the network
stack. Bare-metal PPP clients inherit that contract without the benefits, so
they must take the responsibilities explicitly. When a link is silent, suspect
initialisation and timing before suspecting framing or the peer.

Two things that looked like bugs and were not:

- **MRU=0 is not fatal.** lwIP advertised `02 06 00 00 00 00`, which is an
  invalid MRU, and I was sure the module was rejecting it. It was not: the
  module answered a hand-built request with MRU=0 just as readily as one with
  MRU=1500. `ppp_recv_config()` is not required for this modem.
- **The PPP length field is not required.** RFC 1662 puts a 2-byte length in
  every frame. Both lwIP and the A7670E omit it, and a frame carrying it was
  silently dropped. Match the modem, not the RFC.

And one that nearly cost me the whole diagnosis: **a late-arriving reply to an
earlier command looks like a reply to the current one.** My first raw probe
called `ppp_close()` and then sent a hand-built frame; the 18 bytes it printed
were the modem's Terminate-Ack answering the close, not an answer to my frame.
That fake "REPLY" made a dead link look alive and sent me down the MRU path
for two runs. **When a probe is destructive, never reuse its output as proof
about the next action.**
