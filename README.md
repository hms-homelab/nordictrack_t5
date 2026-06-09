# nordictrack_t5

[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-support-%23FFDD00.svg?logo=buy-me-a-coffee)](https://www.buymeacoffee.com/aamat09)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

ESP32-S3 BLE-to-MQTT bridge for older proprietary-BLE **NordicTrack / iFit treadmills** — Home Assistant auto-discovery.

Connects to the treadmill over its proprietary iFit Bluetooth protocol (advertises as `I_TL`, service `00001533-1412-efde-1523-785feabcd123` — *not* FTMS), replays the host-polled command loop, decodes live telemetry, and publishes it via MQTT. Home Assistant discovers the treadmill automatically — no YAML required. Powered, it stays connected and streams hands-free: walk up, run, data lands in HA.

<p align="center">
  <img src="enclosure/images/enclosure-usbc.jpg" alt="3D-printed enclosure on the NordicTrack console" width="300">
  <img src="homeassistant/images/dashboard-run.jpg" alt="Home Assistant Fitness dashboard" width="300">
</p>

## Features

- Proprietary iFit BLE protocol: GATT discovery, `nordictrack10` init handshake, host-polled **200 ms** loop (nothing is pushed — you must poll)
- Live telemetry decode (page `0x29`): speed, incline, elapsed time, distance — verified against the console (3 mph→482, 5 mph→804, 6 %→300)
- MQTT with Home Assistant auto-discovery (speed, incline, elapsed, distance, moving, connectivity)
- **Weight-aware calories** (ACSM model) using your real smart-scale weight — see [`homeassistant/`](homeassistant/)
- Dual provisioning: SoftAP captive portal **and** BLE GATT — WiFi + MQTT creds stored in NVS
- MQTT-triggered OTA, mDNS (`esp_treadmill.local`), auto-reconnect
- 3D-printed enclosure (OpenSCAD + STLs) with a flush USB-C cutout — see [`enclosure/`](enclosure/)

## Supported Hardware

- **Microcontroller**: Any **ESP32-S3** board (tested on an ESP32-S3 SuperMini), ≥4 MB flash
- **Treadmill**: NordicTrack / ProForm / iFit machines using the older Nordic-Semi proprietary board (advertises `I_TL`, service `1533…`). Decode validated on a T-series console (firmware `2017.0908`).
- **Power**: an always-on USB source (not the treadmill's USB) so the bridge is ready when you step on.

## Prerequisites

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) v5.3 or later
- An MQTT broker (e.g., Mosquitto / EMQX)
- Home Assistant with the MQTT integration enabled

## Build & flash

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py -p /dev/ttyACM0 flash monitor
```

First boot → no creds → it starts SoftAP **`Treadmill-Setup`** (captive portal) and a BLE
provisioning service. Enter WiFi + MQTT (host/port/user/pass); it reboots into RUN mode,
connects to the belt, and Home Assistant auto-discovers the **iFit Treadmill** device.

### ESP32-S3 notes

The cheap SuperMini boards need a little care when running WiFi + BLE together:

- `CONFIG_BT_BTC_TASK_STACK_SIZE=8192` — the default 3072 overflows on the S3 (larger Xtensa frames) during GATT service discovery and panics.
- `CONFIG_SW_COEXIST_ENABLE=y`, and WiFi power-save is disabled at runtime for reliable BLE coexistence.
- `CONFIG_TX_POWER_QDBM` (default 34 ≈ 8.5 dBm) — the on-board PCB antenna overdrives at max power and *reduces* reliability; tune on the bench.

## Home Assistant

The firmware publishes discovery automatically. Add weight-aware calories + a ready-made
**Fitness dashboard** from [`homeassistant/`](homeassistant/) (set your scale's weight entity
in one placeholder).

## Repository layout

| Path | What |
|------|------|
| `main/` | ESP-IDF firmware (Bluedroid GATTC + WiFi + MQTT + OTA + provisioning) |
| `enclosure/` | OpenSCAD rugged-box (single S3 board, flush USB-C) + printable STLs |
| `homeassistant/` | Fitness dashboard + weight-aware calorie templates |
| `docs/` | `TREADMILL_PROTOCOL.md` (verified decode) + `SDD-001..005` design specs |
| `tools/treadmill_poll.py` | Python (bleak) reference reader used to RE & validate the protocol |

## Credits

Protocol reverse-engineered with help from [taylorbowland.com](https://taylorbowland.com)
and [qdomyos-zwift](https://github.com/cagnulein/qdomyos-zwift) (`nordictrack10` family).
Companion deep-dive: the `nordictrack-t5-ble-protocol` repo.

## License

MIT — see [LICENSE](LICENSE).
