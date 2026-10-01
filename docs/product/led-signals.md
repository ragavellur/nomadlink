# NomadLink LED signals

The board has a single WS2812B RGB LED on **GPIO38** (measured by
`hw_selftest.ino`, recorded in `LESSONS_LEARNED.md`; not read off a schematic).
Overridable via NVS key `ls_gpio`; `-1` disables the LED entirely.

This file is the running list of signal meanings. It is deliberately small and
is expected to grow — add a row here **and** a state in
`include/led_strip_status.h` (`led_uplink_state_t`) in the same commit.

## Implemented

| # | Pattern | Meaning | Rate |
|---|---------|---------|------|
| 1 | Fast red blink | 4G selected, no usable network (no signal / not registered / dial failed) | 5 Hz, 100 ms on / 100 ms off |
| 2 | Fast green blink | Connected via **4G** (A7670E PPP link up) | 5 Hz, 100 ms on / 100 ms off |
| 3 | Slow green blink | Connected via **another WiFi SSID** (STA has an IP) | 1 Hz, 500 ms on / 500 ms off |

Notes:

- "Fast" is 200 ms period; "slow" is 1000 ms period. Fast red and fast green
  share a rate but never a colour, and slow green is half the rate of fast
  green, so 4G vs WiFi is readable at a glance.
- Fast red is distinct from the base's pre-existing red, which is a slow (~2 s)
  breathe meaning "no upstream at all". Both are red; the **rate** is the
  separator.
- Priority: factory-reset indication > uplink states (1–3) > base colouring.
- While the modem is still probing/dialing/negotiating, signal 1 stays **off**
  and the base colouring shows — the device does not blink a fault that has not
  happened yet.

## Planned / to be defined (add later)

| # | Pattern | Meaning |
|---|---------|---------|
| 4 | _TBD_ | 4G up but no clients attached / clients attached |
| 5 | _TBD_ | WiFi uplink and 4G both available (failover armed) |
| 6 | _TBD_ | 4G modem present but SIM absent / PIN locked |
| 7 | _TBD_ | Low battery / power fault |

Rows 4–7 are placeholders only. No colour or rate is committed for them yet;
do not implement a guess — get the pattern from the owner first.
