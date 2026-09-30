# Lessons Learned — ESP32-S3 + A7670E (Waveshare V1)

Working reference for this board. Read before touching the modem or GNSS again.

## THE THREE CRITICAL THINGS

> Current lessons run **L-08 … L-15**. If you change the modem power sequence, the NMEA
> port, the AT+CLBS form, or add a new library/core API, read the matching entry first:
> **L-15** ("it compiled" is not a result; a symbol in the archive is not a declaration),
> **L-14** (a compile error in new test code is not a finding about the dependency),
> **L-13** (reproduce the old flow before blaming firmware), **L-12** (lwIP PPP needs
> three things the WiFi stack does not), **L-11** (PWRKEY is not a reset; AT silence is
> ambiguous), **L-10** (a verdict is a claim; the raw bytes are the evidence),
> **L-09** (never write an unverified negative into the docs), **L-08** (a claim in a
> docstring is a claim, not a mechanism).

### 1. The module-enable rail must be asserted — and PWRKEY is not wired

> **Corrected 2026-09-30.** This section previously taught a 1.2 s LOW pulse on
> `MODEM_PWRKEY` = GPIO42, on GPIO21 as the power rail. **Both were a pinout
> misreading.** In the schematic the PWRKEY net does not reach this board, so
> there is no PWRKEY to pulse; the firmware drives **GPIO33** as a
> module-enable rail, asserts it, and waits. PPP reaches the internet on this
> wiring, which is the measurement that settles it. The diagnostic below is kept
> because it is still the right way to recognise a GNSS engine that never
> cold-started — but note that on this board "skipped the pulse" and "never had
> power" are **not** available as separate faults.

The A7670E needs a real power-on sequence. Merely holding the module out of reset
is NOT enough for the GNSS engine.

```cpp
/* What firmware/nomadlink actually does — GPIO33, no PWRKEY pulse. */
pinMode(MODEM_PWR_PIN, OUTPUT);         // GPIO33 = module-enable rail
digitalWrite(MODEM_PWR_PIN, LOW);
delay(100);
digitalWrite(MODEM_PWR_PIN, HIGH);      // assert the rail, then wait for AT
modem.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
```

**Never send a PWRKEY pulse speculatively.** On an already-off module it is a
power *toggle*; on a live one it *aborts* the session. See L-11.

**What happens if the module never actually starts:** it still answers `AT`,
still streams NMEA, and still emits *checksum-valid* sentences — so every naive
test says the GNSS is fine. But it never cold-started, so it never downloads a
fresh almanac. It recites the stale one from flash.

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
- Arduino core **3.3.11**, FQBN (**note the partition scheme — see below**):
  `esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=dio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,PSRAM=enabled,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default`
- **PartitionScheme MUST be `app3M_fat9M_16MB`.** A `default_8MB` build compiles
  perfectly and then does not boot. A compile gate cannot catch this — only
  flashing the target can. (This line previously said `default_8MB`, which is
  why the canonical FQBN lives in `scripts/project-tracker/build-baseline.sh` and
  `flash-sketch.sh`; treat those scripts as the source of truth for the FQBN.)
- Flash the **app partition only** (`0x10000`). The bootloader and partition
  table on this board are known good; overwriting them bricks a booting board.

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

1. **Nearly recorded "ESPAsyncWebServer is incompatible with core 3.3.11" as a
   design constraint, on the strength of one compile error that was actually my own
   call to a non-existent `ESP.getChipId()`.** One symbol, one grep, and the design
   would have been downgraded and written into an ADR. See L-14. General form: a
   compile error names a symbol; it does not name a library.

2. **Skipped the PWRKEY (GPIO42) power-on pulse.** Concluded "no satellite
   signal outdoors is a hardware/antenna problem" when the real cause was the
   module had never been started. Sent the user outdoors for nothing.
3. **Used `CGNSSPORTSWITCH=0,1` instead of `1,1`.** Routed NMEA to the dead USB
   CDC path instead of the live UART.
4. **Used GPIO33 for modem control** because I misread the vendor pinout.
   Correct pins are 21 (power rail) and 42 (PWRKEY). **Superseded 2026-09-30:**
   the schematic says the PWRKEY net does not reach this board, so the firmware
   drives GPIO33 as a module-enable rail and does not pulse a key at all — the
   "21/42" claim above was itself the misreading, and it is now wrong in the
   other direction. Verified by working PPP on this wiring. The real error was
   generalising from one bad pinout read without a measurement; see the
   correction in `AGENTS.md` §5.

5. **Adopted `ESPAsyncWebServer` into an ADR because the spike compiled**, then
   boot-looped on hardware three different ways. `AsyncTCP` calls raw lwIP and
   this core's lwIP asserts on that outside the tcpip thread; `LOCK_TCPIP_CORE`
   trades the assert for a deadlock. See L-15. General form: a green build is a
   statement about the toolchain, not about the device.

6. **Wrote `extern struct netif *esp_netif_get_netif_impl(...)` in a `.ino`** and
   got an "undefined reference" to a symbol that is demonstrably in
   `libesp_netif.a`. A `.ino` is C++; the library is C. `extern "C"` fixed it.
   See L-15.
7. **Invented `AT+CGNSINF`.** It does not exist. `+CME ERROR` from a
   non-existent command looks identical to a hardware failure.
8. **Hand-rolled NMEA parsing, wrong twice**, then reported the garbage output
   as device state. `satsUsed=99` was a parser bug, not a sensor fault.
9. **Gated on `AT+COPS?`**, which is unreliable on this module. Used
   `+CGPADDR` instead.
10. **Gated `SD.begin()` on GPIO46**, which is not a real card-detect line.
   Caused a false FAIL on working hardware.
11. **Did not forward raw NMEA to the host**, leaving no ground truth to
   re-analyse when results looked wrong.
12. **Ran `AT+CNMP=2` (2G) expecting a fallback.** Airtel retired 2G. There is no
   2G fallback on this SIM; CNMP must stay 38.
13. **Set `AT+CNMI=2,1` expecting delivery reports**, but that leaves status
    reports off. Needed `2,1,0,1,0`.
14. **`PIN_RX`/`PIN_TX` collided with IDF macros**, producing a nonsense error
    inside `soc/reg_base.h`.
15. **Asserted a "no carrier" conclusion from `AT+COPS?`** and told the user the
    board was faulty, when the module was fine and the command was the problem.
16. **Declared cellular LBS unsupported from failed invocations alone.** Never ran
    `AT+CLBS=?`, which shows the real signature and that the CID is required. I
    told the user twice that a working feature was missing, and told them so
    after they had already said it worked. Overrode the user's direct
    observation of prior hardware behaviour in favour of my own failed test.
17. **Blamed the wrong subsystem for a socket problem.** `CIPSTART` fails on this
    module; `CIPOPEN` works. I reported the whole cellular path as unusable
    because I'd only tried the command the TinyGSM driver doesn't use.
18. **Compared results across differing hardware states.** The patch antenna was
    connected for part of the LBS testing and not the rest. `+CSQ: 16` -> `26`.
    I treated results spanning different antenna states as firmware facts.
19. **Wrote unverified negatives into the docs**, which compounded a guess into
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

## L-13 — When something that worked stops working, reproduce the old flow
   byte-for-byte before touching anything (2026-09-30)

`AT+CLBS=1,1` returned a real position on 09-29 and `+CLBS: 10` (close network
error) on 09-30, same device, same SIM, same place. The instinct is to start
changing the firmware. That instinct was wrong, and the way to kill it is
mechanical: **replay the exact command sequence that previously succeeded,
unchanged, on the committed control sketch.**

The committed `lbsmatrix` sketch *is* that control. It failed identically on
its own, before any experimental variant was run. That single result moved the
question from "which of my changes broke it?" to "what changed outside the
firmware?", which is the only question worth asking.

Two follow-ups that closed it:

- **A true cold power-cycle is not an ESP32 reset.** The modem had been up all
  session being churned with `CFUN` cycles and PDP-context deletes. The only
  way to rule out a wedged long-lived session is to actually power the module
  off (long PWRKEY hold) and back on. A USB reset leaves the modem running.
- **Prove the data plane separately from the application.** `NETOPEN` +
  `CIPOPEN` to a public host — and specifically to `lbs-simcom.com:3002`, the
  LBS server itself — all returned OK, while only the `AT+CLBS` transaction
  closed. A76XX has no `AT+CLBSCFG`, so the server address cannot be moved in
  firmware. The failing boundary was the LBS service, on their side.

The same day GNSS went to zero satellites. The signature mattered more than the
number: **the engine was powered, NMEA kept streaming, and C/N0 never started.**
That is RF or sky-view, never a dead engine or a parser bug. A PASS earlier in
the day and a total failure later is not a paradox, it is a moved device.

General rule: **an intermittent external-service failure is proven by the
unchanged control, not by the newest experiment.** If the control that used to
pass still passes, the regression is yours. If it now fails, the regression is
the world's, and no amount of firmware work will find it. Also: when two
independent subsystems fail in the same window, that is a strong hint they share
a cause — here, a portable unit that was moved and a carrier service that went
down. Check the physical setup before the build.


---

## L-14 — A compile error in new test code is not a finding about the dependency (2026-09-30)

Adding the admin console, the first spike of `ESPAsyncWebServer` + `AsyncTCP`
against the production FQBN failed on its first attempt. The tempting
conclusion was that the async libraries are incompatible with core 3.3.11, which
would have sent me to the documented fallback (the core's blocking `WebServer.h`)
and permanently downgraded the design on the strength of one error message.

The error was `ESP.getChipId()`. That API does not exist in this core. The
libraries were never involved — I had called a function that was never there.
After removing it the same spike compiled clean: 948,319 bytes flash, 47,300
bytes RAM.

General rule: **before concluding that a dependency is incompatible, read the
error's own claim.** A compile error names a symbol, a type or a signature; it
does not name a library. Check whether the symbol belongs to the dependency or to
the code you just typed, by searching the library headers and the core headers
separately. This is the third time this project has mis-read a failure in
*test* code as a fact about hardware or a third-party component (see L-08, and
the `MARK-CONNECTED` marker that printed regardless of CONNECT success).

The same increment produced three more of these, all worth the same scepticism:
`WiFi.softAPgetStationInfo()` simply no longer exists in core 3.x,
`ESP.getMinEverFreeHeap()` is named `ESP.getMinFreeHeap()`, and
`esp_wifi_ap_get_sta_list_with_ip()` is not reachable from `esp_wifi.h` — it
lives in `esp_wifi_ap_get_sta_list.h`, which must be included *after*
`esp_wifi.h` or it deliberately fails with a misleading "WiFi header mismatch!".
None of those are compatibility problems. Read the header, do not theorise.

Worth stating plainly, because it is the reason this is written down: the
four-verified-dependencies claim would have been just as false as the
one-verified-dependencies claim it replaced, and it would have been recorded in
an ADR as a design constraint. **A gate that only runs the new code is not
enough — a new API must be confirmed against the headers that ship with the
core, not against a tutorial written for a different core version.**

## L-15 — "It compiled" is not a result, and a symbol in the archive is not a declaration (2026-09-30)

Two failures on the same day, both about the gap between *building* and
*working*, and both of the kind this project keeps paying for.

**A spike that compiles proved nothing.** ADR-007 adopted `ESPAsyncWebServer`
on evidence that included a clean compile. It boot-looped on hardware. The
cause was not a version mismatch or a missing API: `AsyncTCP` calls **raw
lwIP** (`tcp_listen_with_backlog()` → `tcp_alloc()`), and this core is built
with `CONFIG_LWIP_CHECK_THREAD_SAFETY=y` and
`CONFIG_LWIP_TCPIP_CORE_LOCKING_INPUT` **unset**, so `LWIP_ASSERT_CORE_LOCKED()`
only passes on the tcpip thread. Three arrangements were tried — setup task,
`LOCK_TCPIP_CORE()`, and a dedicated FreeRTOS task — and all three abort,
because the restriction is in the core's **lwIP build**, not in scheduling.
Wrapping the call in `LOCK_TCPIP_CORE()` trades the assert for a deadlock,
because `AsyncTCP` uses a *blocking* `tcpip_api_call()` while the tcpip thread
needs the very lock being held.

**The rule:** when a library bypasses an API layer, the question is what the
*core was built with*, not which version is installed. Read the sdkconfig. And
note the reference project never made this choice safe — it uses
`esp_http_server`, a different library, so its immunity was never evidence for
`ESPAsyncWebServer` in the first place.

**A symbol in the archive is not a declaration.** `esp_netif_get_netif_impl()`
is defined in `libesp_netif.a` and absent from `esp_netif.h`. Declaring it in a
`.ino` — which is compiled as **C++** — mangles the call to
`_Z24esp_netif_get_netif_implP13esp_netif_obj`, and the link fails with an
"undefined reference" to a name that is visibly present in the archive. The
fix is `extern "C"`. The reference project never hit this because
`netif_hooks.c` is C. This is L-14's family one level deeper: grep the archive
**and** the headers, and check the **linkage**, not just the spelling.

Both were caught by hardware, both after the code "passed" its gate. A compile
gate proves the toolchain is willing, which is a much smaller claim than it
looks like.

**What now catches it:** `make check` builds every sketch, and hardware boot is
a separate, explicit step that writes a log rather than an impression. The
lesson is not to let a green build stand in for a boot.

## L-16 — Check whether the thing already exists before rebuilding it (2026-10-01)

A prototype firmware was written for the NomadLink network stack that reproduced,
feature for feature, `esp32_nat_router` — a project that already existed, was
maintained, was already running on this board, and was already flashed by the
project owner. Around 1,300 lines of Arduino were written to recreate
`ip_napt_enable()` routing, a DHCP server, packet hooks, a web console and
per-client statistics.

**The wrong turn:** the brief was "make it work like the reference", and that was
read as "implement the same behaviours". The reference itself was never opened
before writing the code. By the time it was, the whole stack existed already.

**Why it was worse than wasted effort — it regressed known-good behaviour.**
ADR-008 had already recorded that `ESPAsyncWebServer` aborts under forwarding
load on Arduino 3.3.11, because AsyncTCP drives raw lwIP from the wrong thread
and `LOCK_TCPIP_CORE()` deadlocks instead. Hand-rolling the Arduino equivalent
walked straight back into that recorded defect, because **the reference is immune
only because it uses `esp_http_server`**. The immunity was never evidence for
AsyncTCP — L-15 says exactly that, and the rebuild ignored it. Every hour spent
writing code was an hour not spent reading a working implementation.

**The general rule.** Before implementing a subsystem, answer three questions in
this order:

1. **Does a working implementation of this already exist?** Search before
   designing. If it exists and is running on the target, it is the base, not a
   spec.
2. **Is it already on the device?** A firmware the owner has flashed and reports
   working is the strongest evidence available — stronger than any sketch
   compiled on a desk.
3. **What does inheriting it cost?** Vendoring is cheap: `cp -R`, plus a
   provenance record and a licence check. Re-implementing costs the defect class
   the original already solved, and the features that were actually being asked
   for.

"Reference implementation" means *use it*, not *match its behaviour*. Reading
the reference is the cheapest step in software engineering and it was skipped.

**Two smaller traps in the same commit, both caught only by checking:**

- **`.gitignore` had `*.bin`.** The web installer's four firmware parts were
  silently excluded from git. The page and its manifest were correct, the
  manifest resolved, the button mounted — and on the published site **every
  binary would have 404'd**. A locally served page proves nothing about a
  published one. *Check the ignore rules for files you intend to commit*, and
  prefer an assertion (`git check-ignore`) over a visual scan.
- **A compile gate that reported success without building anything.** It grepped
  the build log for `error:` instead of checking the exit code, and ran `idf.py`
  from the repository root, so it failed with `CMakeLists.txt not found` — a
  string the grep did not match — and printed `ok`. It was fixed, then proven
  fixed by injecting invalid C and confirming a non-zero exit naming the file and
  line. That is L-08 applied to a gate: **an untriggered guard is not a guard.**

**What now catches it:** `make product` builds `firmware/nat_router` unmodified
and exits non-zero on a broken tree; `firmware/nat_router/README.md` records
upstream, version, commit and licence; ADR-009 states the base must compile
*before* any feature is layered on, so a base that only builds after local edits
is visible as a base that has been forked.

### L-16 addendum — a correct page can serve a stale artifact (2026-10-01)

The rebrand of L-16 immediately produced the same failure in a new place, which
is why it is recorded here rather than as a separate lesson.

The AP SSID was renamed `ESP32_NAT_Router` → `NomadLink` in the source, and the
flashed device still broadcast the old name. Not a missed edit: **the installer
was serving upstream's prebuilt binaries.** The page rendered, the manifest
resolved, every part returned HTTP 200, every gate was green — and none of that
touched the source, because those bytes were never built from this repository.

**The rule: verify the artifact, not the intention.** For anything shipped to a
user, a passing check on the *page* proves only that the page is correct. It says
nothing about whether the payload is the thing you edited. The check that
actually matters reads the staged bytes:

```
python3 - <<'PY'
d = open('docs/project/firmware/nomadlink.bin','rb').read()
for s in (b'NomadLink', b'ESP32 NAT Router'):
    print(s, d.count(s))
PY
```

Grepping the source would have said the rename worked. Grepping the artifact is
the only check that could have caught this.

**What now catches it.** `make stage-firmware` owns the path from source to the
bytes a user flashes, so it cannot be forgotten silently; `make product` warns
when the staged payload is older than the sources it came from, naming the file;
and `stage-firmware.sh` asserts the four flash offsets against
`build_esp32s3/flash_args` before copying, because a wrong offset flashes cleanly
and then does not boot — the failure mode that "the installer worked" would not
have caught.

Two habits generalise. **Name the artifact after your product**, which here meant
renaming the IDF project (the output filename derives from it) rather than
renaming a file after the build. And **let the brand be wrong everywhere at once
or nowhere** — the SSID, the artifact, the UI title, the console banner and the
provenance line were fixed together, because a partial rebrand is harder to spot
than none.

## L-17 — Never hand over an instruction until the change is pushed and verified live (2026-10-01)

The user opened `https://ragavellur.github.io/nomadlink/project/index.html`,
found no **Connect & Install** button, and correctly concluded the work was not
done. It wasn't. **20 commits had never been pushed.** `git log @{u}..HEAD` was
non-empty the whole time, going back to `981e635`, and the published site was
days stale.

**The wrong turn:** I finished the work, committed it, wrote a confident
"push and try the page whenever you're ready", and treated pushing as the user's
job. Every local signal was green — `make check` passed, the button mounted in
Chrome, all four binaries returned 200 from a local `http.server` — so the
summary said the work was done. None of those signals can observe the
difference between a local file and a published one. I had a remote configured
and never pushed to it.

**The general rule: a local commit is not a delivered change.** Nothing reaches
the device, the web installer, or the user until it is on `origin/main`. The
sequence is not optional and not parallel:

| Step | Command | Proof |
|---|---|---|
| 1 | `make stage-firmware` | four staged sizes printed |
| 2 | `make render` | "Rendered 8 dashboard pages" |
| 3 | `git commit` | non-empty SHA |
| 4 | `git push origin main` | `main -> main`, no error |
| 5 | `curl -s -o /dev/null -w '%{http_code}'` the real URL | `200` and the expected size |
| 6 | **then** ask the user to test | — |

**Do not hand over a URL before step 5.** If `git log @{u}..HEAD` is non-empty
you have unfinished work, not a deliverable — no matter how good the local build
is. Say "I have not pushed yet", finish it, then give the instruction. The
user is tracking a flash, not reading a report; every minute they spend on a
stale page is a minute lost, and they reasonably conclude the feature is
missing rather than that the delivery is.

This is L-16 in the delivery pipeline rather than the code: the installer served
a *prebuilt* binary no source edit could reach, and then served a *stale
repository* no local verification could reach. **A check that cannot fail on the
real artifact is not evidence about the real artifact.** A local `http.server`
proves the HTML is well-formed and the paths are right; it proves nothing about
what users will see. The only checks that do are `git log @{u}..HEAD` (is it
pushed) and `curl` against the public URL (is it served).

**What now catches it:** `AGENTS.md` §0 carries the six-step table and states that
no instruction may be given while the unpushed list is non-empty. Push is an
agent step, not a user step, and "it works locally" is never a reason to stop
short of step 5.
