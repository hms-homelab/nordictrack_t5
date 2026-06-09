# SDD-003 — WiFi, MQTT & Home Assistant Discovery

Status: DRAFT · Parent: SDD-001 · esp-idf v5.3.4 · 2026-06-06

## Goal
RUN-mode networking: connect WiFi STA from NVS creds, advertise mDNS `esp_treadmill`,
connect MQTT, publish HA auto-discovery configs once, then stream telemetry with an
availability (LWT) signal. Auto-reconnect on every layer (WiFi / MQTT / BLE).

## Interfaces
- **WiFi STA**: SSID/pass from NVS (SDD-004); WiFi/BLE software coexistence on (SDD-001).
- **mDNS**: hostname `esp_treadmill` → `esp_treadmill.local`; service `_treadmill._tcp` (port 80 if a diag page is served, else just hostname).
- **MQTT broker**: host/port/user/pass from NVS; client-id `esp_treadmill-<mac6>`.

## Topics (prefix `<P>` = Kconfig topic prefix, default `treadmill`)
| Topic | Dir | Payload |
|-------|-----|---------|
| `<P>/status` | pub (LWT + birth) | `online` / `offline` (retained) |
| `<P>/state` | pub | JSON telemetry (on-change or ~1 Hz) |
| `homeassistant/<comp>/<P>/<obj>/config` | pub (retained) | HA discovery per entity |
| `<P>/cmd/ota` | sub | OTA trigger (see SDD-005) |

## Data formats
**State JSON** (`<P>/state`):
```json
{ "speed_mph": 3.00, "incline_pct": 3.0, "elapsed_s": 42,
  "distance_mi": 0.021, "moving": true, "connected": true }
```
Speed converted km/h→mph on-device (×0.6214); distance integrated from speed·dt.

**HA discovery** (one retained config per entity, `device` block shared = "iFit Treadmill"):
| Entity | component | device_class | unit | value_template |
|--------|-----------|--------------|------|----------------|
| Speed | sensor | speed | mph | `{{ value_json.speed_mph }}` |
| Incline | sensor | — | % | `{{ value_json.incline_pct }}` |
| Elapsed | sensor | duration | s | `{{ value_json.elapsed_s }}` |
| Distance | sensor | distance | mi | `{{ value_json.distance_mi }}` |
| Moving | binary_sensor | running | — | `moving` true/false |
| Connected | binary_sensor | connectivity | — | `connected` |

All configs carry `availability_topic: <P>/status`, `payload_available: online`.

## Acceptance criteria
- HA shows device "iFit Treadmill" with all 6 entities via discovery, no manual YAML.
- Values track the belt live; speed shown in mph.
- Treadmill off → BLE down → `connected=false`; entities reflect it. Broker-side
  loss → `<P>/status` = `offline` via LWT → entities `unavailable`.
- `ping esp_treadmill.local` resolves on the LAN.
- WiFi drop → auto-reconnect; MQTT drop → auto-reconnect + re-publish birth `online`
  (discovery is retained, no re-publish needed but harmless if re-sent).

## Out of scope
TLS/MQTTS (plain TCP v1); inbound control entities (read-only telemetry).
