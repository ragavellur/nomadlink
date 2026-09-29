# NomadLink One — Software Architecture

**Status:** draft, pending ADR-005 (firmware framework). The layer boundaries
below are framework-independent; only the implementation language is undecided.

## 1. Why this shape

The device has one hard constraint that shapes everything: **the cellular
modem owns the network path.** The A7670E is a modem with its own TCP/IP stack,
not a network interface the MCU can borrow. Two consequences:

1. Sockets are created with `AT+CIPOPEN`, not a POSIX socket call, unless PPP
   succeeds and gives us a real interface (TASK-020, RISK-002).
2. NAT routing for tethered clients requires a working IP interface on the MCU.
   That is exactly what PPP would provide, which is why TASK-020 gates REQ-004.

Every other module sits above that boundary and must not care which of the two
paths we end up with. The `Transport` abstraction exists for that reason.

## 2. Layers

```
┌──────────────────────────────────────────────────────────────┐
│  NomadLink Console (web portal)                              │
│  Dashboard · Network · Camera · Location · Cloud · SMS · Voice│
└───────────────────────────┬──────────────────────────────────┘
                            │ HTTP (on-device ESPAsyncWebServer)
┌───────────────────────────┴──────────────────────────────────┐
│  Services                                                     │
│  TelemetryPublisher   CommandEngine   LocationService        │
│  SmsEngine            CallManager     WakeWordEngine         │
│  NetworkManager       StorageService  CameraService          │
└───────────────────────────┬──────────────────────────────────┘
                            │ typed interfaces
┌───────────────────────────┴──────────────────────────────────┐
│  Drivers                                                      │
│  ModemClient · GnssDriver · LbsDriver · CameraDriver ·       │
│  SdDriver · I2sAudioDriver · Storage (NVS/LittleFS)          │
└───────────────────────────┬──────────────────────────────────┘
                            │ UART / AT command layer
┌───────────────────────────┴──────────────────────────────────┐
│  A7670E modem (own TCP/IP stack)                              │
│  AT+CGPSINFO · AT+CLBS · AT+CIPOPEN · AT+CMGS · ATD · AT+CSSLCFG│
└──────────────────────────────────────────────────────────────┘
```

## 3. The Transport seam

Everything that needs the internet depends on `Transport`, never on the modem
directly.

```cpp
class Transport {
  virtual bool connect(const char* host, uint16_t port, bool tls) = 0;
  virtual size_t write(const uint8_t* data, size_t len) = 0;  // exact byte count
  virtual int    read(uint8_t* buf, size_t cap, uint32_t timeout_ms) = 0;
  virtual void   disconnect() = 0;
  virtual bool   isConnected() const = 0;
};
```

Two implementations:

- **`AtCipTransport`** — `AT+CIPOPEN` / `AT+CIPSEND` over the modem's stack.
  Proven to open a socket (`+CIPOPEN: 0,10`). Not yet proven to carry an MQTT
  CONNECT (BUG-002).
- **`PppTransport`** — a lwIP socket over the PPP interface, if TASK-020
  succeeds. Preferred when available: it gives NAT routing for free and lets us
  use a normal TLS stack instead of `AT+CSSLCFG`.

Choosing the firmware framework (ADR-005) determines how easily each is built.
That decision is deliberately still open, and TASK-020 is the input to it.

## 4. Why the modem is not treated as a network device

This is the mistake the reference router invites. `esp32_nat_router` assumes
PPP and hands the result to lwIP. On this hardware PPP has never produced an IP
(`+CGREG: 0,3`), so that architecture would have nothing to work with.

The design above treats the modem as what it verifiably is: a device that
accepts `AT` commands and can open its own TCP sockets. PPP support is added
later, behind the same `Transport` seam, as an optimisation and a route to NAT
— not as a prerequisite.

## 5. Location

`LocationService` owns one `Position` struct with an explicit `source` field
(`LBS` or `GNSS`) and an accuracy value. It does not pretend a cell-tower fix is
a GPS fix.

Per ADR-006 the hierarchy is: publish an LBS fix immediately (fast, coarse,
always available), then upgrade to GNSS once `AT+CGPSINFO` reports
`quality >= 1` and `sats_used > 0`. Downgrades happen only on a configurable
timeout, so a brief GNSS dropout does not flap the reported position.

The GNSS engine must be started once and left powered. Re-powering it mid-session
prevents an almanac download, and the module will still emit checksum-valid NMEA,
which looks like a fix but is not. The driver therefore latches a `gnss_started`
flag and refuses to re-init the engine within a session.

## 6. Telemetry and commands

`TelemetryPublisher` owns a `Transport` and publishes a fixed JSON payload:

```json
{"device":"nomadlink-one","lat":18.480776,"lon":73.897998,"alt":512.0,
 "speed":0.0,"sats":17,"fix_quality":1,"source":"GNSS","acc":3,
 "rssi":26,"operator":"Airtel","uptime":86400,"batt_mv":3980}
```

`source` is always present. A consumer must be able to tell a cell-tower
estimate from a satellite fix without inferring it from the accuracy field.

`CommandEngine` is the inverse: subscribe, parse
`{command_id, action, params}`, dispatch, publish
`{command_id, status, result}`. The `command_id` must be echoed so a client can
correlate without guessing. This is TASK-407/408, sequenced after the transport
works.

Per ADR-004 there is no relay, bridge or cloud service in this path. The device
talks to the broker and the broker is reachable by the client.

## 7. Security

- No credential in source, ever. `tracker/secrets.h` is gitignored and the
  template carries no values.
- `scripts/project-tracker/secrets_scan.py` runs in the commit gate.
- A credential that was ever committed or shared in plaintext is **rotated**,
  not just deleted.
- The portal requires HTTP Basic Auth (TASK-404).
- Broker transport is TLS 8883 via `AT+CSSLCFG` with SNI, not plaintext 1883
  (TASK-804). This is currently the weakest link: TLS is unproven on this
  firmware (RISK-005), so plaintext shipping is not acceptable.
- The ESP32's NVS keys are obfuscated, not encrypted. This protects against
  casual extraction, not a determined attacker with physical access.

## 8. Concurrency

One task owns the UART and all AT traffic. Every driver sends a request and waits
on a queue; none of them touch the serial port directly. This is required even
though the work is currently single-threaded, because the moment a SoftAP
server, a telemetry publisher and an SMS engine run together, interleaved `AT`
commands will corrupt each other.

## 9. What this architecture deliberately does not do

- No cloud backend service. Telemetry is device-to-broker, per ADR-004.
- No OTA update in v0.1.0. It needs a verified secure transport first.
- No abstraction over "the modem is also a GNSS receiver". The GNSS is reached
  through the same UART but is a genuinely different concern with different
  timing rules.

## 10. Open items

| Question | Resolved by |
|---|---|
| ESP-IDF or Arduino for the product firmware? | ADR-005, informed by TASK-020 |
| Is a real IP interface available at all? | TASK-020 / RISK-002 |
| Are the I2S mic and DAC present? | TASK-021 / RISK-003 |
| Does `AT+CSSLCFG` TLS work? | TASK-804 / RISK-005 |
| What NAT throughput is real? | TASK-110 / RISK-001 |
