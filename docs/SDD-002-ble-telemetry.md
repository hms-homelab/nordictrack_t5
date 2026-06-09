# SDD-002 — BLE Telemetry (Bluedroid GATTC Central)

Status: DRAFT · Parent: SDD-001 · Stack: esp-idf v5.3.4 · Bluedroid GATTC · 2026-06-06

## Goal
RUN-mode component that scans for `I_TL`, connects as a Bluedroid GATT **client**,
replays the iFit init handshake, runs the 200 ms poll state machine, decodes the
live `0x29` page, and exposes a thread-safe `telemetry_t`. Read-only (no control).
Source of truth: `TREADMILL_PROTOCOL.md` (VERIFIED LIVE DECODE).

## Interfaces (GATT)
| Item | Value |
|------|-------|
| Scan match | device name `I_TL` (macOS shows CoreBluetooth UUID — match by name) |
| Service | `00001533-1412-efde-1523-785feabcd123` |
| Notify char | `00001535-…` (handle 0x000B) — subscribe via CCCD 0x000C = `0x0100` |
| Write char | `00001534-…` (handle 0x000E), write-with-response |
| TX power | clamped via Kconfig (`esp_ble_tx_power_set`), not max — see SDD-001 |

## Sequence
1. GATTC register → set scan params → `esp_ble_gap_start_scanning`.
2. On name match: open connection (`esp_ble_gattc_open`), `MTU` exchange (≥ 23 ok; frames are 20 B).
3. Discover service `1533`; resolve handles for `1534`/`1535` + CCCD.
4. Register for notify (`esp_ble_gattc_register_for_notify`) + write CCCD `0x0100`.
5. Replay `nordictrack10` init handshake (20-B frames to 0x000E, ~400–600 ms apart) — verbatim, see protocol doc.
6. Start `esp_timer` @ **200 ms** driving `counterPoll` 0→5; one write/tick (poll group `FE021403` etc.).
7. On notify: filter to live frames, decode, update `telemetry_t` under mutex.

## Data formats
**Live frame** `0x29`: 20 bytes starting `00 12 01 04 02`; reject if bytes 12–19 all `0xFF` (stale).
| Field | Bytes (LE) | Formula | Unit |
|-------|-----------|---------|------|
| Speed | [10],[11] | `(b11<<8 \| b10)/100` | km/h (×0.6214 → mph) |
| Incline | [12],[13] | `(int16)(b13<<8 \| b12)/100` | % |
| Elapsed | [14] | counter | s |

```c
typedef struct {
  float speed_kmh;   // raw/100
  float incline_pct;
  uint8_t elapsed_s;
  bool link_up;      // GATTC connected
  int64_t last_rx_us;
} telemetry_t;       // guarded by mutex; consumed by mqtt_manager
```

## Acceptance criteria
- UART log decodes match the belt calibration: raw 482 → 3.00 mph, 804 → 5.00 mph, incline 300 → 3.0%.
- Elapsed counter increments while moving.
- 200 ms poll holds a stable link for ≥ 75 s (no ~10–20 s idle drop).
- Treadmill power-off → `link_up=false` within reconnect timeout; auto re-scan/reconnect on power-on.
- C decode is byte-for-byte identical to `artifacts/treadmill_poll.py` output for the same session.

## Out of scope
Speed/incline **control** (`FE020D02` write path + checksum) — deferred; fragment
reassembly of long config messages (not needed for live page).
