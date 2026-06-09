# SDD-005 — MQTT-Triggered OTA

Status: DRAFT · Parent: SDD-001 · esp-idf v5.3.4 · `esp_https_ota` · 2026-06-06

## Goal
Update RUN-mode firmware over the air without USB: publish a firmware URL to an MQTT
command topic; device pulls via `esp_https_ota`, applies to the inactive OTA slot,
reports progress/result over MQTT, and is rollback-safe.

## Partitioning
- Two app OTA slots (`ota_0` / `ota_1`) + `otadata`; factory optional.
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` → mark valid only after self-test passes
  (WiFi+MQTT reconnect within timeout), else auto-rollback on next boot.

## Interfaces / topics (prefix `<P>` from NVS, default `treadmill`)
| Topic | Dir | Payload |
|-------|-----|---------|
| `<P>/cmd/ota` | sub | JSON `{ "url": "https://host/fw.bin", "sha256": "<hex>" }` (sha256 optional) |
| `<P>/ota/status` | pub (retained) | JSON state (below) |

**Status JSON** (`<P>/ota/status`):
```json
{ "state": "downloading", "progress": 42, "version": "1.0.3", "error": null }
```
`state` ∈ `idle | downloading | applying | success | failed | rolled_back`;
`progress` 0–100; `error` string on failure.

## Flow
1. Subscribe `<P>/cmd/ota` after MQTT connect.
2. On message: validate URL (https), publish `downloading`, run `esp_https_ota` (stream, with progress callback → throttled `progress` updates).
3. Optional sha256 verify of received image; on success `applying` → `esp_ota_set_boot_partition` → reboot.
4. After reboot self-test ok → `esp_ota_mark_app_valid_cancel_rollback` → publish `success` + new `version`.
5. Self-test fail / download fail / bad image → `failed` (or `rolled_back` after watchdog reboot).

## Acceptance criteria
- Publishing a valid URL updates firmware end-to-end; reported `version` changes.
- Progress advances 0→100 during download.
- Corrupt/unreachable image → `failed`, device stays on current firmware (no brick).
- Image that fails self-test → automatic rollback → `rolled_back`, previous firmware runs.
- OTA never runs in PROV mode (RUN-only; coex — SDD-001).
- `<P>/ota/status` retained so late HA subscribers see last result.

## Out of scope
Signed/secure-boot image verification (v1 relies on https + optional sha256);
delta/compressed OTA; scheduled/automatic update checks.
