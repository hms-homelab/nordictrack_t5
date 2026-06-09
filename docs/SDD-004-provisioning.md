# SDD-004 — Provisioning (Op-Mode Split: SoftAP Portal + Bluedroid GATTS)

Status: DRAFT · Parent: SDD-001 · esp-idf v5.3.4 · Bluedroid GATTS · 2026-06-06

## Goal
Runtime provisioning of WiFi + MQTT creds into NVS with **no reflash**. Boot-time
op-mode select decides PROV vs RUN. Two parallel PROV paths share the same NVS
schema: (a) SoftAP captive portal, (b) Bluedroid GATT **server**. Never run the
central (RUN) and the prov server in the same boot (coex — SDD-001).

## Op-mode boot logic (`main.c`)
```
boot → read NVS op_mode + wifi creds
  if force-prov button held (long-press)  → PROV
  elif no valid wifi creds                → PROV
  else                                    → RUN
```
Re-provision: portal "forget" or BLE `clear` cmd → wipe creds → set op_mode=PROV → reboot.

## NVS schema (namespace `cfg`, mirrors cpapdash nvs_config API)
| Key | Type | Notes |
|-----|------|-------|
| `wifi_ssid` / `wifi_pass` | str | required for RUN |
| `mqtt_host` / `mqtt_port` | str/u16 | broker |
| `mqtt_user` / `mqtt_pass` | str | optional |
| `topic_prefix` | str | default `treadmill` |
| `op_mode` | u8 | 0=auto-RUN, 1=force-PROV |

## Interfaces
**(a) SoftAP captive portal**
- AP SSID `Treadmill-Setup` (Kconfig), open or WPA2 per Kconfig; DNS catch-all → 192.168.4.1.
- HTTP form: WiFi SSID/pass, MQTT host/port/user/pass, topic prefix → write NVS → reboot to RUN.

**(b) Bluedroid GATTS prov service** (advertises while in PROV)
| Char | Access | Payload |
|------|--------|---------|
| WiFi creds | write | `ssid\0pass` |
| MQTT creds | write | `host\0port\0user\0pass` |
| Apply/cmd | write | `apply` (commit+reboot RUN) / `clear` (wipe+PROV) |
| Status | read/notify | `prov` / `saved` / `err` |

## Acceptance criteria
- Fresh device (empty NVS) boots PROV: `Treadmill-Setup` AP up **and** GATTS prov service advertising.
- Portal form save → reboot → RUN connects WiFi+MQTT with saved creds.
- BLE prov write+`apply` → same result, no reflash.
- Creds survive power cycle.
- Long-press force-prov button on a provisioned device → re-enters PROV.
- `clear` (portal forget or BLE) wipes creds and returns to PROV on next boot.

## Out of scope
Encrypted prov payloads / pairing PIN (v1 plaintext on a trusted LAN/short prov window);
cloud/phone-app provisioning.
