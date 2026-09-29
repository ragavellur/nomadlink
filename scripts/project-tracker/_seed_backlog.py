#!/usr/bin/env python3
"""Append the forward-referenced TASK-* and TEST-* records, then re-render."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BP = ROOT / "docs/project/backlog.json"
TP = ROOT / "docs/project/test-results.json"

TS = {"created_at": "2026-09-29T00:00:00Z", "started_at": "", "completed_at": "",
      "verified_at": "", "last_updated_at": "2026-09-29T00:00:00Z"}


def task(tid, title, typ, epic, feat, reqs, pri, deps, desc, tests):
    return {
        "id": tid, "title": title, "type": typ, "epic_id": epic, "feature_id": feat,
        "requirement_ids": reqs, "priority": pri, "status": "BACKLOG", "dependencies": deps,
        "description": desc,
        "acceptance_criteria": [
            f"Implementation satisfies the acceptance criteria of {', '.join(reqs) or 'its parent feature'}",
            "Hardware behaviour is demonstrated on NomadLink One and captured as evidence",
            "Documentation updated and commit SHA recorded",
        ],
        "definition_of_done": [
            "Code implemented and formatted", "Static checks pass", "Unit tests pass where logic exists",
            "Applicable integration/E2E tests pass", "Acceptance criteria individually satisfied",
            "Documentation updated", "Git commit created", "Commit SHA recorded",
            "Verification evidence recorded", "Project JSON and HTML updated and committed",
            "No known blocking defect",
        ],
        "test_ids": tests,
        "verification": {"status": "NOT_VERIFIED", "evidence": [], "verified_by": "", "verified_at": "", "reason": ""},
        "git": {"commit_sha": "", "branch": "main"}, "timestamps": dict(TS),
    }


NEW_TASKS = [
    task("TASK-105", "Wi-Fi STA credential persistence and auto-reconnect on boot", "feature", "EPIC-001", "FEAT-002", ["REQ-002"], "P0", ["TASK-102"],
         "Load stored STA credentials from NVS at boot and auto-associate, falling back to cellular when no STA link is established.", ["TEST-102"]),
    task("TASK-106", "Automatic APN detection and configuration", "feature", "EPIC-001", "FEAT-003", ["REQ-003"], "P1", ["TASK-103"],
         "Detect APN from the SIM (IMSI MCC/MNC or a lookup table) and configure AT+CGDCONT automatically, with a manual override in the web UI.", []),
    task("TASK-107", "Network status monitoring surface (RSSI, registration, operator)", "feature", "EPIC-001", "FEAT-003", ["REQ-003"], "P1", ["TASK-103"],
         "Expose CSQ, CEREG/CGREG state and COPS operator to the web dashboard. Must also surface antenna state, because forgetting the 4G patch antenna silently breaks LBS.", ["TEST-103"]),
    task("TASK-108", "WAN health evaluation and failover state machine", "feature", "EPIC-001", "FEAT-003", ["REQ-004"], "P0", ["TASK-101", "TASK-103"],
         "Continuously evaluate STA link health and LTE readiness, and drive a state machine that selects the active WAN. This is the control half of REQ-004.", ["TEST-105"]),
    task("TASK-109", "LwIP NAT forwarding between SoftAP clients and the active WAN", "feature", "EPIC-001", "FEAT-003", ["REQ-004"], "P0", ["TASK-108"],
         "Implement packet forwarding and NAT translation. Blocked on RISK-002: requires a real IP interface, which most likely means PPP must work first.", ["TEST-104"]),
    task("TASK-110", "Throughput and failover-time measurement harness", "performance", "EPIC-001", "FEAT-003", ["REQ-004"], "P0", ["TASK-109"],
         "Measure actual NAT throughput and failover time and compare against the PRD NFRs of 10-15 Mbps and under 3 s. Produce evidence for a renegotiation decision. See RISK-001.", ["TEST-104", "TEST-105"]),
    task("TASK-201", "Camera driver and frame capture with PSRAM buffers", "feature", "EPIC-002", "FEAT-004", ["REQ-005"], "P1", ["TASK-019"],
         "Bring up the OV2640 on the V1 pinout, allocate frame buffers in PSRAM, and expose frame capture. Sensor-level capture is already VERIFIED; the streaming pipeline is not.", ["TEST-201"]),
    task("TASK-202", "MJPEG encode and HTTP /stream endpoint", "feature", "EPIC-002", "FEAT-004", ["REQ-005"], "P1", ["TASK-201"],
         "Encode frames as MJPEG and stream them over multipart HTTP to connected clients.", ["TEST-201"]),
    task("TASK-203", "Camera control page: resolution, quality, FPS, snapshot", "feature", "EPIC-002", "FEAT-005", ["REQ-006"], "P2", ["TASK-202"],
         "Build the /camera page with interactive controls for resolution, JPEG quality, frame rate and snapshot capture.", ["TEST-202"]),
    task("TASK-302", "Position service exposing lat/lon/alt/speed/sats with a source field", "feature", "EPIC-003", "FEAT-006", ["REQ-007"], "P0", ["TASK-301"],
         "Single position struct with an explicit source (GNSS or LBS) and accuracy. Implements the ADR-006 hierarchy: publish LBS immediately, upgrade to GNSS when quality >= 1 and sats_used > 0.", ["TEST-301", "TEST-302"]),
    task("TASK-304", "Location web page with Leaflet/OpenStreetMap map", "feature", "EPIC-003", "FEAT-008", ["REQ-009"], "P2", ["TASK-302"],
         "Live lat/lon/alt/speed/sats display plus an embedded interactive Leaflet map. The map must visually distinguish an LBS fix from a GNSS fix, per ADR-006.", ["TEST-303"]),
    task("TASK-401", "Cloud broker credential UI (HiveMQ and Adafruit IO)", "feature", "EPIC-004", "FEAT-009", ["REQ-010"], "P1", ["TASK-404"],
         "Form to enter broker URL, port, client id, username, password and topics for HiveMQ, plus the equivalent Adafruit IO fields. Values go to NVS, never to source.", ["TEST-401"]),
    task("TASK-402", "NVS-backed configuration store", "feature", "EPIC-008", "FEAT-020", ["REQ-010", "REQ-021"], "P0", ["TASK-019"],
         "Persist Wi-Fi credentials, broker credentials, APN, contacts, voice triggers and the SMS whitelist in NVS/LittleFS. Replaces the current hardcoded-credential pattern.", ["TEST-802"]),
    task("TASK-403", "Telemetry destination selector and publish interval", "feature", "EPIC-004", "FEAT-010", ["REQ-011"], "P1", ["TASK-406"],
         "Destination selector (Disabled / HiveMQ / Adafruit IO), Start/Stop controls and a configurable interval from 5 to 3600 seconds.", ["TEST-402"]),
    task("TASK-404", "Web portal authentication and secret handling", "security", "EPIC-008", "FEAT-021", ["REQ-022"], "P0", ["TASK-019"],
         "Enforce HTTP Basic Auth on the admin portal, ensure no secret is ever returned in a page or log, and adopt the NVS store so no secret lives in source.", ["TEST-803"]),
    task("TASK-407", "MQTT command subscription and JSON parser", "feature", "EPIC-004", "FEAT-012", ["REQ-013"], "P1", ["TASK-405"],
         "Subscribe to the command topic, parse command_id/action/params JSON, and dispatch to handlers. Depends on TASK-405 completing first.", ["TEST-404"]),
    task("TASK-408", "Command execution and response publishing", "feature", "EPIC-004", "FEAT-012", ["REQ-013"], "P1", ["TASK-407"],
         "Execute GET_STATUS / TOGGLE_ROUTER / REBOOT / SEND_SMS / DIAL_NUMBER and publish a matching result to the response topic.", ["TEST-404"]),
    task("TASK-501", "SMS receive interception via +CMTI with whitelist enforcement", "feature", "EPIC-005", "FEAT-013", ["REQ-014"], "P1", ["TASK-103"],
         "Intercept +CMTI URCs, read the message with AT+CMGR, and reject any sender not on the whitelist. Outbound SMS is already VERIFIED; inbound interception is not.", ["TEST-501"]),
    task("TASK-502", "SMS whitelist management surface", "feature", "EPIC-005", "FEAT-013", ["REQ-014"], "P2", ["TASK-501"],
         "Web UI to add and remove authorised numbers, stored in NVS.", []),
    task("TASK-503", "SMS command parser and auto-reply", "feature", "EPIC-005", "FEAT-014", ["REQ-015"], "P2", ["TASK-501"],
         "Case-insensitive STATUS / LOCATION / STREAM ON / STREAM OFF / REBOOT / CALL <number> parsing with automatic reply to the sender.", ["TEST-502"]),
    task("TASK-601", "Incoming call detection with auto-answer, reject and whitelist", "feature", "EPIC-006", "FEAT-015", ["REQ-016"], "P1", ["TASK-103"],
         "Detect RING/+CLIP, apply configurable auto-answer after N rings, reject unauthorised callers with ATH, and surface an Accept/Hang Up popup.", ["TEST-601"]),
    task("TASK-602", "Web telephony control surface and call status", "feature", "EPIC-006", "FEAT-015", ["REQ-016"], "P2", ["TASK-601"],
         "Real-time call state, duration display and the Accept/Hang Up popup in the web portal.", []),
    task("TASK-603", "Web dialpad and contact-based dialling", "feature", "EPIC-006", "FEAT-016", ["REQ-017"], "P2", ["TASK-602"],
         "Dialpad UI and saved contacts issuing ATD<number>; over the API. ATD dialling is already VERIFIED at the AT level.", ["TEST-602"]),
    task("TASK-604", "Full-duplex I2S audio path (mic capture and DAC playback)", "feature", "EPIC-006", "FEAT-016", ["REQ-017"], "P2", ["TASK-019"],
         "I2S microphone capture and I2S DAC playback routed for calls. BLOCKED on RISK-003: the presence and wiring of the audio hardware on this board is unconfirmed.", ["TEST-602"]),
    task("TASK-701", "ESP-SR engine with latency benchmark", "feature", "EPIC-007", "FEAT-017", ["REQ-018"], "P2", ["TASK-019"],
         "ESP-SR WakeNet + MultiNet running fully offline, with a measured latency benchmark against the PRD sub-800ms NFR. BLOCKED on RISK-003.", ["TEST-701"]),
    task("TASK-702", "Wake-word detection with audio feedback", "feature", "EPIC-007", "FEAT-017", ["REQ-018"], "P2", ["TASK-701"],
         "Wake-word recognition with a confirmation beep played through the DAC, then open a recognition window.", []),
    task("TASK-703", "Voice contacts directory and voice-activated dialling", "feature", "EPIC-007", "FEAT-018", ["REQ-019"], "P3", ["TASK-702"],
         "Web UI to manage contacts and phonetic triggers, then the wake-word -> 5s window -> match -> verbal confirm -> ATD sequence.", ["TEST-702"]),
    task("TASK-801", "ESPAsyncWebServer shell and navigation layout", "feature", "EPIC-008", "FEAT-019", ["REQ-020"], "P0", ["TASK-019"],
         "Async web server with the seven-section navigation: Dashboard, Network, Camera, Location, Cloud, SMS, Telephony.", ["TEST-801"]),
    task("TASK-802", "Dashboard view: network, GPS quick view, system stats", "feature", "EPIC-008", "FEAT-019", ["REQ-020"], "P1", ["TASK-801"],
         "Landing page showing WAN mode, Wi-Fi signal, 4G RSSI, operator, fix status, lat/lon, free RAM, PSRAM usage and minimum-ever-free-heap.", ["TEST-801"]),
    task("TASK-803", "Responsive layout and styling across all pages", "feature", "EPIC-008", "FEAT-019", ["REQ-020"], "P2", ["TASK-801"],
         "Responsive CSS and consistent component styling across every portal page.", ["TEST-801"]),
    task("TASK-804", "TLS transport for cloud connectivity", "security", "EPIC-008", "FEAT-021", ["REQ-022"], "P0", ["TASK-405"],
         "Configure AT+CSSLCFG with SNI and certificate installation so MQTT runs over TLS 8883 rather than plaintext 1883. BLOCKED on RISK-005: untested on this firmware.", ["TEST-803"]),
    task("TASK-805", "SD card mount and storage integration", "feature", "EPIC-008", "FEAT-022", ["REQ-023"], "P2", ["TASK-019"],
         "Mount the card and use it for snapshots, GNSS track logs and device logs. BLOCKED on BUG-001: works under Arduino, fails under ESP-IDF with err=0x107.", ["TEST-804"]),
    task("TASK-020", "Experiment: PPP data path on the Airtel SIM", "spike", "EPIC-000", "FEAT-000", ["REQ-004"], "P0", ["TASK-001"],
         "Run RISK-002. The earlier PPP failure (+CGREG: 0,3, CGACT 1,0) was on a Jio SIM and looks like a provisioning symptom, not a hardware fault. Retest on the Airtel SIM, which is proven to attach and yield an IP via CGPADDR. Outcome decides whether REQ-004 NAT routing is achievable at all.", ["TEST-020"]),
    task("TASK-021", "Experiment: confirm audio hardware population on the board", "spike", "EPIC-000", "FEAT-000", ["REQ-017", "REQ-019"], "P0", ["TASK-001"],
         "Run RISK-003. Physically inspect whether the I2S MEMS microphone and I2S DAC are populated, and if so run a mic-to-DAC loopback test. nomad-sentinel TASKS.md 1.11 records the audio path as UNSUPPORTED. Outcome decides whether ESP-SR and full-duplex voice descope.", ["TEST-021"]),
    task("TASK-022", "Experiment: GPS cold-start benchmark harness", "spike", "EPIC-000", "FEAT-000", ["REQ-007"], "P1", ["TASK-001"],
         "Repeat the cold-start measurement several times to characterise the real distribution of time-to-first-fix, rather than relying on a single 69 s observation. Feeds the indoor-usability and failover design.", ["TEST-301"]),
]

NEW_TESTS = [
    ("TEST-020", "PPP establishes and assigns an IP on the Airtel SIM.", "integration", "REQ-004", "TASK-020",
     ["Airtel SIM attached", "APN airtelgprs.com configured"],
     ["Bring up PPP over UART", "Observe PPP Start/Establish/Auth/Network", "Confirm LCP/IPCP negotiation", "Confirm an IP is assigned"],
     "PPP establishes and an IP is assigned", "",
     "NOT_RUN", "", ""),
    ("TEST-021", "I2S audio hardware is present and a mic-to-DAC loopback carries signal.", "hardware_regression", "REQ-017", "TASK-021",
     ["Physical board inspection"],
     ["Inspect the board for a populated I2S mic and I2S DAC", "If present, record from the mic and play to the DAC", "Measure the loopback signal"],
     "Audio hardware confirmed present and functional, or confirmed absent so the feature can descope", "",
     "NOT_RUN", "", ""),
    ("TEST-101", "A client associates to the SoftAP and receives a DHCP lease.", "integration", "REQ-001", "TASK-101",
     ["SoftAP running"],
     ["Associate a client", "Check for a 192.168.4.x lease", "Request the captive portal path"],
     "Client receives a lease and the captive portal redirects", "", "NOT_RUN", "", ""),
    ("TEST-102", "Stored Wi-Fi STA credentials persist across a reboot and auto-reconnect.", "integration", "REQ-002", "TASK-105",
     ["Credentials saved to NVS"],
     ["Reboot the device", "Confirm STA auto-associates", "Confirm fallback to cellular if no STA link"],
     "STA auto-associates after reboot", "", "NOT_RUN", "", ""),
    ("TEST-201", "MJPEG frames are delivered to an HTTP client streaming from /stream.", "end_to_end", "REQ-005", "TASK-202",
     ["Camera capturing"],
     ["Request /stream", "Confirm a multipart MJPEG response", "Confirm frames advance"],
     "A browser receives a live MJPEG stream", "", "NOT_RUN", "", ""),
    ("TEST-202", "Camera control page applies resolution, quality and FPS changes at runtime.", "end_to_end", "REQ-006", "TASK-203",
     ["/camera page available"],
     ["Change resolution", "Change JPEG quality", "Change FPS", "Trigger a snapshot"],
     "Each control takes effect and the snapshot downloads", "", "NOT_RUN", "", ""),
    ("TEST-303", "Location page shows live position and the map marker moves.", "end_to_end", "REQ-009", "TASK-304",
     ["Position service producing fixes"],
     ["Open /location", "Confirm lat/lon/alt/speed/sats update", "Confirm the Leaflet marker tracks position", "Confirm LBS and GNSS fixes are visually distinguished"],
     "Live values update and the marker tracks position", "", "NOT_RUN", "", ""),
    ("TEST-401", "Broker credentials entered in the web UI are stored and used for a connection.", "integration", "REQ-010", "TASK-401",
     ["NVS store available"],
     ["Enter broker credentials in the UI", "Reboot", "Confirm the values persist", "Confirm a connection is attempted with them"],
     "Credentials persist across reboot and are used", "", "NOT_RUN", "", ""),
    ("TEST-402", "Telemetry start/stop and interval control publish rate.", "integration", "REQ-011", "TASK-403",
     ["A broker connection available", "An independent subscriber observing the topic"],
     ["Start telemetry at a 5 s interval", "Confirm publishes arrive at that rate", "Change the interval to 60 s", "Confirm the rate changes", "Stop telemetry and confirm publishes cease"],
     "Publish rate follows the configured interval and stops on demand", "", "NOT_RUN", "", ""),
    ("TEST-404", "An MQTT command received on the command topic is executed and acknowledged.", "end_to_end", "REQ-013", "TASK-408",
     ["Broker connection with subscribe enabled", "An external publisher on the command topic"],
     ["Publish a GET_STATUS command", "Confirm a matching command_id response on the response topic", "Repeat for REBOOT and DIAL_NUMBER"],
     "Each command executes and a SUCCESS response with a matching command_id is published", "", "NOT_RUN", "", ""),
    ("TEST-502", "SMS commands are parsed and answered, and unauthorised senders are ignored.", "end_to_end", "REQ-015", "TASK-503",
     ["SMS receive enabled", "A whitelisted and a non-whitelisted sender available"],
     ["Send STATUS from a whitelisted number", "Confirm a reply", "Send LOCATION and confirm a Maps link", "Send a command from a non-whitelisted number and confirm it is ignored"],
     "Whitelisted commands are executed and answered; non-whitelisted senders get no action and no reply", "", "NOT_RUN", "", ""),
    ("TEST-602", "A call can be placed from the web dialpad and the audio path carries voice.", "end_to_end", "REQ-017", "TASK-604",
     ["RISK-003 resolved: audio hardware confirmed present"],
     ["Dial a number from the web dialpad", "Confirm the call reaches active state", "Speak and confirm the far end hears audio", "Hang up"],
     "The call connects and two-way audio is confirmed", "", "NOT_RUN", "", ""),
    ("TEST-701", "ESP-SR wake-word and command recognition latency is under the 800 ms NFR.", "performance", "REQ-018", "TASK-701",
     ["RISK-003 resolved", "ESP-SR models loaded"],
     ["Measure wake-word detection latency over 20 runs", "Measure command-phrase classification latency over 20 runs", "Report p50 and p95"],
     "Both latencies are under 800 ms, or the NFR is formally renegotiated against the measurement", "", "NOT_RUN", "", ""),
    ("TEST-702", "A voice command dials the mapped contact.", "end_to_end", "REQ-019", "TASK-703",
     ["Wake word and command phrases trained", "Contacts configured"],
     ["Say the wake word", "Confirm the audio beep", "Say a contact trigger within the 5 s window", "Confirm the verbal confirmation and the dialled call"],
     "The mapped contact is dialled after the spoken trigger", "", "NOT_RUN", "", ""),
    ("TEST-801", "Every portal page loads and renders its live data.", "smoke", "REQ-020", "TASK-802",
     ["Web server running"],
     ["Load each of the seven sections", "Confirm live values update without a manual reload", "Confirm the layout is responsive at mobile width"],
     "All pages load, update live, and are responsive", "", "NOT_RUN", "", ""),
    ("TEST-802", "Configuration persists across reboot with no secret in source.", "integration", "REQ-021", "TASK-402",
     ["NVS store implemented"],
     ["Set Wi-Fi, broker and APN configuration", "Reboot", "Confirm all values persist", "Run the secrets scanner over the tree and history"],
     "All values persist and the secrets scanner passes", "", "NOT_RUN", "", ""),
    ("TEST-803", "Security controls hold: admin auth enforced, no secret exposure, TLS in use.", "security", "REQ-022", "TASK-804",
     ["Basic Auth enforced", "TLS configured"],
     ["Request the portal without credentials and confirm rejection", "Confirm no password appears in any page or log", "Confirm the broker connection is TLS 8883, not plaintext 1883"],
     "Unauthenticated access is rejected, no secret leaks, and transport is encrypted", "", "NOT_RUN", "", ""),
]

backlog = json.loads(BP.read_text())
existing = {t["id"] for t in backlog["tasks"]}
added = 0
for t in NEW_TASKS:
    if t["id"] not in existing:
        backlog["tasks"].append(t)
        added += 1
BP.write_text(json.dumps(backlog, indent=2) + "\n")

tr = json.loads(TP.read_text())
ex = {t["id"] for t in tr["tests"]}
for tid, desc, typ, req, tsk, pre, steps, exp, act, status, ev, ts in NEW_TESTS:
    if tid in ex:
        continue
    tr["tests"].append({
        "id": tid, "description": desc, "type": typ, "requirement": req, "task": tsk,
        "preconditions": pre, "steps": steps, "expected_result": exp, "actual_result": act,
        "status": status, "evidence": ev, "executed_at": ts,
    })
TP.write_text(json.dumps(tr, indent=2) + "\n")
print(f"added {added} tasks, {len(NEW_TESTS) - (len(ex))} tests")
print(f"tasks now: {len(backlog['tasks'])}, tests now: {len(tr['tests'])}")
