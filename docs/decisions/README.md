# NomadLink — Architecture Decision Records

Decisions are immutable once accepted. Superseding a decision creates a new ADR.

| ADR | Title | Status | Date |
|---|---|---|---|
| [ADR-001](ADR-001-repository-as-source-of-truth.md) | Repository is the source of truth | Accepted | 2026-09-29 |
| [ADR-002](ADR-002-hardware-facts-override-prd.md) | Verified hardware facts override the PRD | Accepted | 2026-09-29 |
| [ADR-003](ADR-003-json-source-of-truth-html-generated.md) | JSON is authoritative, HTML is generated | Accepted | 2026-09-29 |
| [ADR-004](ADR-004-direct-device-to-broker-mqtt.md) | Direct device-to-broker MQTT, no relay | Accepted | 2026-09-29 |
| [ADR-005](ADR-005-firmware-framework-selection.md) | Firmware framework selection | **Superseded by ADR-009** | 2026-09-29 |
| [ADR-006](ADR-006-location-source-hierarchy.md) | Location source hierarchy: LBS then GNSS | Proposed | 2026-09-29 |
| [ADR-007](ADR-007-admin-console-web-stack.md) | Admin console: async web server, direct-IP access, deferred auth | **Superseded by ADR-009** | 2026-09-30 |
| [ADR-008](ADR-008-admin-console-core-webserver.md) | Admin console: core `WebServer`, and a selectable uplink | **Superseded by ADR-009** | 2026-09-30 |
| [ADR-009](ADR-009-vendored-nat-router-base.md) | Product firmware is the vendored `esp32_nat_router` base, ESP-IDF 5.5.x | Accepted | 2026-10-01 |
