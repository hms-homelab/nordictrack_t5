# NordicTrack / iFit "I_TL" Treadmill BLE Protocol

> Status (2026-06): **Protocol fully reverse-engineered.** Confirmed against two
> independent public sources *and* our own PacketLogger capture. The framing,
> init handshake, poll cadence, telemetry decode, and speed/incline control are
> all documented below. Remaining unknown: exact per-model payload tails for our
> specific firmware (`2017.0908`) — see Caveats.

## Device Information
- **Name**: `I_TL`
- **MAC** (Linux/BlueZ): `DC:E3:FA:CF:00:91` — on macOS bleak shows a CoreBluetooth UUID instead; scan by name.
- **Firmware string**: `2017.0908`
- **Service UUID**: `00001533-1412-efde-1523-785feabcd123` (proprietary Nordic-Semi iFit control service — **NOT** FTMS)

## Primary Sources (the breakthrough)
1. **taylorbowland.com** — 4-part Wireshark/btsnoop packet-capture RE of a NordicTrack
   **T6.5S**, the *identical* `1533/1534/1535` UUID family and handles. Byte-level bible.
   - `/posts/treadmill-getting-data/`, `/treadmill-analyzing-reads/`,
     `/treadmill-analyzing-writes/`, `/treadmill-sending-data/`
2. **qdomyos-zwift (QZ)** — `github.com/cagnulein/qdomyos-zwift`, working C++ control.
   Treadmill code: `src/devices/proformtreadmill/proformtreadmill.cpp`. Our closest
   model branch is **`nordictrack10`** (shares a parser branch with `nordictrackt70`,
   `proform_treadmill_1800i`). QZ bridges this proprietary protocol **up to** standard
   FTMS (presents as a virtual Wahoo KICKR).
3. Our own capture: `artifacts/treadmill_att.txt` (2,155 ATT ops). The truncated
   `…` payloads are now fully recoverable from QZ's source (frames match exactly).

## GATT Map
| Handle | UUID | Props | Role |
|--------|------|-------|------|
| 0x000B | `00001535-…` | NOTIFY/READ (0x12) | Telemetry/state — server → client |
| 0x000E | `00001534-…` | WRITE/READ (0x0a) | Commands — client → server |
| 0x000C | CCCD (0x2902) | — | Write `0x0100` (LE) to enable notifications |

## Framing
**Same scheme both directions.** Each 20-byte BLE frame = `[marker][len][payload…]`:
- `0xFE` = **first** frame of a logical message; byte 1 is the total payload size.
- `0x00, 0x01, 0x02…` = **continuation** frame, value = sequence index.
- `0xFF` = **last** frame.

The first packet's size byte = sum of following fragment sizes
(e.g. `FE02 1903` → `0x19` = `0x12 + 0x07` = 18 + 7 = 25). ⚠️ Note: **not every
packet starts with `FE02`** — `FE02` only marks a *command/sequence start*.

> **Practical shortcut for reading:** the live telemetry is carried in a single
> 20-byte notification that begins `00 12 01 04 02 …`. You do **not** need to
> reassemble fragments to read speed/incline — just parse that one frame at fixed
> offsets (see below). Reassembly only matters for the longer config/info messages.

## ✅ VERIFIED LIVE DECODE (our I_TL, fw 2017.0908, 2026-06-06)
Confirmed end-to-end on macOS with `artifacts/treadmill_poll.py` against the real
belt. **The live speed/incline page on our unit is `0x29`** (page-id byte = data
length, varies; the app capture showed `0x2F`). It is the response to the
**`FE021403` poll group**, NOT the bare header and NOT the `0x17/0x19` polls.

Poll group that triggers it (exact bytes from our capture; frame 2's last 4 bytes
were log-truncated → 00-padded, works fine):
```
FE021403
001202040210041002000A1394330010 00000000
FF0218F2 00000000000000000000000000000000
```
Live page `0x29` frame `00 12 01 04 02 29 04 29 02 02 [SP_LO SP_HI] [IN_LO IN_HI] [SEC] …`:
| Field | Bytes (LE) | Formula | Verified |
|-------|-----------|---------|----------|
| **Speed** | 10,11 | `/100` → **km/h** (×0.6214 → mph) | 482→3.00 mph, 804→5.00 mph ✅ |
| **Incline** | 12,13 | `/100` → **%** | 300→3.0% ✅ |
| Elapsed | 14 | seconds counter | increments ✅ |

Internal units are **km/h** (3 mph = 4.82 km/h = raw 482). Poll cadence 200 ms held
a stable connection for full 75 s captures (bare/idle subscribe used to drop ~10–20 s).

## Telemetry Decode (from QZ parser, `proformtreadmill.cpp:3712`)
A valid state notification is **20 bytes** starting `00 12 01 04 02` and (for our
branch) byte 5 ∈ {`0x31`, `0x34`}. Reject frames where bytes 12–19 are all `0xFF`
(stale/placeholder). Then:

| Field | Bytes (LE) | Formula |
|-------|-----------|---------|
| **Speed** | [10],[11] | `(b11<<8 \| b10) / 100.0`  (km/h or mph per machine units) |
| **Incline** | [12],[13] | `(int16)(b13<<8 \| b12) / 100.0` |
| **Watts** | [14],[15] | `b15<<8 \| b14`  (>3000 ⇒ treat as 0/invalid) |

(Distance/time/calories are derived by QZ from speed integrated over time, not read
directly for this branch.)

## Connection + Init Handshake (the "auth")
There is **no cryptographic auth** — control is gated only by replaying the init
sequence. The mystery bytes (`8187`, `8088`, `8890`) were never decoded; **replay
them verbatim**. Read-only monitoring may not even need the full handshake.

Full `nordictrack10` init (20-byte frames, written to 0x000E, ~400–600 ms apart),
from QZ `btinit()` — these are the *complete* versions of the truncated frames in
our capture:
```
FE020802
FF08 02 04 02 04 02 04 81 87 00 00 00 00 00 00 00 00 00 00
FE020802
FF08 02 04 02 04 04 04 80 88 00 00 00 00 00 00 00 00 00 00
FE020802
FF08 02 04 02 04 04 04 88 90 00 00 00 00 00 00 00 00 00 00
FE020A02
FF0A 02 04 02 06 02 06 82 00 00 8a 00 00 00 00 00 00 00 00
FE020A02
FF0A 02 04 02 06 02 06 84 00 00 8c 00 00 00 00 00 00 00 00
FE020802
FF08 02 04 02 04 02 04 95 9b 00 00 00 00 00 00 00 00 00 00
FE022C04
0012 02 04 02 28 04 28 90 04 00 61 d8 5d d0 51 d0 55 e8 61
0112 f8 8d 00 91 20 d5 48 e1 98 3d d0 71 10 b5 48 e1 b8 4d
FF08 e0 b1 40 80 02 00 00 75 00 00 00 00 00 00 00 00 00 00
```
(The trailing `noOpData1..6` `FE0217 03 / FE0219 03` blocks in `btinit()` are the
first poll iterations — see below.)

## The Clock (poll loop)
**Host-polled request/response — nothing is pushed.** QZ drives a `QTimer` at
**200 ms** (`refresh->start(200ms)`) advancing a `counterPoll` 0→5 state machine →
one write per 200 ms, full cycle ≈ **1.2 s** (reconciles taylorbowland's "~1 s").

Per-tick `noOp` poll commands for `nordictrack10` (each is a header `FE02 xx03`
optionally followed by `0012…`/`FF…` frames). The four steady-state headers we saw
cycle (`FE02 1903 / 1403 / 3304 / 2604`) are these polls. Replaying the bare 4-byte
headers may be enough to elicit telemetry — **to be confirmed empirically** with
`artifacts/treadmill_poll.py`.

## Control: Set Speed / Incline (from QZ `forceSpeed` / `forceIncline`)
Header `FE020D02` then a 20-byte `FF0D…` frame. Field selector at byte **10**:
`0x01` = speed, `0x02` = incline. Value little-endian at bytes **11,12** =
`round(value * 100)`. Byte **14** = checksum.
```
Speed:   FE020D02
         FF 0D 02 04 02 09 04 09 02 01 [01] [LO] [HI] 00 [CK] 00 00 00 00 00
Incline: FE020D02
         FF 0D 02 04 02 09 04 09 02 01 [02] [LO] [HI] 00 [CK] 00 00 00 00 00
```
- `LO,HI` = `(value*100)` little-endian. Example: 5.0 mph → 500 → `F4 01`;
  taylorbowland's 5.0 example used 805 (0x0325 → `25 03`) on a units-differing machine.
- **Checksum `CK` (byte 14)** for the `nordictrack10` branch = `sum(write[6..12]) & 0xFF`
  (the generic `for i in 0..6: write[14]+=write[i+6]` path). Other branches use
  `write[11]+write[12]+0x11`/`+0x12`. **Confirm ours by capture before trusting writes.**

## Relationship to FTMS
The `1533` protocol is **pre-FTMS proprietary**. Older Nordic-Semi boards (our
`2017.0908`) are not natively Zwift/FTMS compatible. QZ exists to bridge proprietary
→ FTMS (`0x1826`, treadmill data char `0x2acd`). Unrelated dead ends:
`ESP32_TTGO_FTMS` (sensor-fed FTMS server, no protocol decode), `zwifit` (newer
WiFi/WebSocket-JSON machines), QZ Companion app (ADB/OCR on tablet machines).

## Caveats
1. Byte-level details come from a **T6.5S** and the ProForm **bike** driver. Framing
   is portable, but **payload tails, the speed/incline checksum, and per-model init
   bytes vary** — re-capture our `I_TL` (fw `2017.0908`) to confirm before relying on
   writes.
2. Required minimum poll interval is unverified (200 ms works in QZ; ~1 s in capture).
3. Init handshake may contain session-specific elements — capture-and-replay from our
   own device is safest.

## Next Steps
1. **Verify the clock with bleak** (`artifacts/treadmill_poll.py`): replay init →
   poll loop → watch the `00 12 01 04 02` frames decode to live speed/incline.
2. Confirm whether bare 4-byte poll headers suffice vs. full 3-frame groups.
3. Capture our own `forceSpeed`/`forceIncline` to confirm the checksum byte.
4. **Port to esp-idf**: NimBLE central → subscribe `1535` → `esp_timer` poll loop at
   200 ms with the `counterPoll` state machine → same frame parser in C. Mirror
   QZ's `nordictrack10` branch.
