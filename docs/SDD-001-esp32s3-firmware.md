# SDD-001 — ESP32-S3 iFit Treadmill → MQTT Bridge Firmware

Status: DRAFT (spec) · Target: generic ESP32-S3 · esp-idf v5.3.4 · 2026-06-06

## Goal
Headless ESP32-S3 firmware that acts as a BLE central to the NordicTrack/iFit
`I_TL` treadmill, drives the polling protocol we reverse-engineered, decodes live
telemetry, and publishes it to MQTT with Home Assistant auto-discovery. Runtime
provisioning (WiFi + MQTT) via captive portal stored in NVS — no reflash to config.
BLE stack is **Bluedroid** (the esp-idf default classic+BLE host), not NimBLE — see
the stack note below. Sub-specs: BLE telemetry SDD-002, WiFi/MQTT/discovery SDD-003,
provisioning SDD-004, MQTT OTA SDD-005.

## Protocol (already solved — see TREADMILL_PROTOCOL.md, VERIFIED section)
- Connect to `I_TL`, service `00001533-…`; notify `1535`/0x000B, write `1534`/0x000E.
- Replay `nordictrack10` init handshake (FE020802/FF08… etc.), then poll @200 ms.
- Poll group (drives live page 0x29): `FE021403` / `001202040210041002000A1394330010`(+00 pad) / `FF0218F2`+0.
- Decode page 0x29 frame `00 12 01 04 02 29 04 29 02 02 …`:
  speed = LE[10,11]/100 km/h; incline = LE[12,13]/100 %; byte14 = elapsed sec.

## Decisions (locked)
| Topic | Choice |
|------|--------|
| Board | Generic ESP32-S3 devkit (no board peripherals; status via GPIO LED if wired) |
| Provisioning | Runtime, **dual**: (a) SoftAP captive portal + (b) BLE GATT prov — both write WiFi + MQTT creds to NVS |
| Units | mph (speed), % (incline); km/h converted on-device |
| Discovery | HA MQTT discovery + availability (LWT) + auto-reconnect (WiFi/MQTT/BLE) |
| OTA | **MQTT-triggered OTA**: publish firmware URL to `<prefix>/cmd/ota`; `esp_https_ota` pull+apply; status back over MQTT |
| BLE host | **Bluedroid** (esp-idf default GATTC/GATTS host). Not NimBLE. |

### BLE stack & role note (Bluedroid)
We use **Bluedroid** as the BLE host (`CONFIG_BT_BLUEDROID_ENABLED=y`, BLE-only —
classic BT disabled). It exposes the BLE 4.2 GATTC API for the central role
(treadmill) and GATTS for the provisioning peripheral. Bluedroid running a GATT
client and a GATT server concurrently on one radio causes scheduling churn /
disconnects, so we **mode-split** (same as the NimBLE rationale, just on Bluedroid):
boot to **PROV mode** (SoftAP portal + Bluedroid GATTS server) when NVS has no WiFi
creds or a button/long-press forces it; otherwise boot to **RUN mode** (Bluedroid
GATTC central + WiFi + MQTT). Re-provision = clear NVS (portal "forget" or BLE cmd)
→ reboot. Mirrors cpapdash-push-c3 op_mode pattern.

**S3 BT/coex hardening (locked):**
- **Op-mode boot split** is the primary coexistence lever — never run GATTC scan and
  the SoftAP/GATTS provisioning server in the same boot.
- **WiFi/BLE coexistence** in RUN mode: WiFi STA + BLE central share the 2.4 GHz
  radio. Enable software coexistence (`CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y`); keep
  WiFi modem-sleep on so BLE poll windows (200 ms) are honored. Treadmill link is the
  latency-sensitive path; MQTT publishes tolerate jitter.
- **S3 BTC/controller stack sizing:** bump the BT controller + BTC task stacks above
  defaults (Bluedroid on S3 is stack-hungry under GATTC+notify load); set BLE max
  connections = 1 for RUN mode to cap controller RAM.
- **TX-power hardening:** clamp BLE (and WiFi) TX power rather than running at max —
  the on-board/PCB antenna can overdrive and *reduce* link reliability (same lesson
  as the C3 Zero, where max power was worse). Use `esp_ble_tx_power_set()` to a
  moderate level and tune empirically; expose it via Kconfig.
- **mDNS:** advertise as `esp_treadmill` (hostname `esp_treadmill.local`,
  service `_treadmill._tcp`) in RUN mode so the bridge is discoverable on the LAN for
  diagnostics/OTA URLs without knowing the DHCP address.

## Entities (HA discovery, device "iFit Treadmill")
| Entity | Class | Unit | Source |
|--------|-------|------|--------|
| Speed | sensor | mph | page0x29 speed (km/h×0.6214) |
| Incline | sensor | % | page0x29 incline |
| Elapsed | sensor (duration) | s | byte14 counter |
| Distance | sensor | mi | integrated speed·dt on-device |
| Moving | binary_sensor (running) | — | speed > 0 |
| Connected | binary_sensor (connectivity) | — | BLE link up (also drives availability/LWT) |

## Architecture (mirrors cpapdash-push-c3; MQTT from apc-usb-mqtt-bridge)
Template files to port: Bluedroid GATTC central ← `cpapdash-push-c3/miner/main/o2ring_ble.c`
+ `scanner_task.c` (port the NimBLE logic to the Bluedroid GATTC API); BLE prov ←
`mule/main/ble_prov.c` (→ Bluedroid GATTS); portal ← `captive_portal.c`;
NVS ← `nvs_config.c/h`; OTA ← `ota_handler.c`; WiFi ← `wifi_manager.c`; MQTT+discovery
← `apc-usb-mqtt-bridge/main/mqtt_manager.c`.
```
treadmill-s3/main/
  main.c             op_mode select (PROV vs RUN); BT controller+Bluedroid init; event glue
  nvs_config.c/h     wifi + mqtt creds, op_mode (mirror cpapdash API)
  ble_treadmill.c/h  RUN: Bluedroid GATTC central — scan I_TL, connect, handshake, 200ms poll, parse 0x29 → telemetry_t
  wifi_manager.c/h   RUN: STA connect + reconnect; mDNS esp_treadmill
  mqtt_manager.c/h   RUN: connect, HA discovery once, publish telemetry, LWT
  ota_handler.c/h    RUN: MQTT cmd → esp_https_ota, progress/result over MQTT
  ble_prov.c/h       PROV: Bluedroid GATTS server → write creds to NVS
  captive_portal.c/h PROV: SoftAP + DNS + HTTP form → NVS
  led_status.c/h     PROV/connecting/streaming indication (configurable GPIO)
  Kconfig.projbuild  LED GPIO, poll interval, AP SSID/pass, topic prefix, force-prov button, BLE TX power
```
Telemetry flows BLE task → `telemetry_t` (mutex) → MQTT publisher (on change or ~1 Hz).

## Phasing (each independently testable)
1. **BLE core** — Bluedroid GATTC central: scan/connect/handshake/poll/parse; log telemetry over UART. Proves the C port matches the Python reader. (detail: SDD-002)
2. **WiFi+MQTT** — STA from NVS (hardcoded fallback first), mDNS `esp_treadmill`, MQTT publish + HA discovery + LWT. (detail: SDD-003)
3. **Provisioning** — dual: SoftAP captive portal **and** Bluedroid GATTS prov → NVS; op_mode boot select; force-prov button; reconnect/robustness. (detail: SDD-004)
4. **OTA** — MQTT cmd topic → `esp_https_ota`; rollback-safe; status/progress published back. (detail: SDD-005)

## Test / acceptance
- Phase 1: UART log shows speed/incline matching the belt (re-use the calibration: 3 mph→4.82 km/h, 6%).
- Phase 2: HA shows the device + entities via discovery; values track the belt; treadmill off → entities `unavailable`.
- Phase 3: fresh device boots SoftAP `Treadmill-Setup`; portal saves; survives reboot & WiFi/BLE drops.

## Out of scope (v1)
Speed/incline **control** (write path / forceSpeed) — read-only telemetry first. The
control encoding is documented (FE020D02 + checksum) for a later SDD. FTMS rebroadcast
(Zwift) also deferred.
