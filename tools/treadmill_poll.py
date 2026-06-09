#!/usr/bin/env python3
"""
NordicTrack / iFit "I_TL" treadmill — instrumented BLE poller.

Goal: nail down the CLOCK. This board is host-polled request/response, not
push. The phone writes tiny command frames to the WRITE char (0x000E) and the
treadmill answers with a burst of notification frames on the NOTIFY char
(0x000B). Nothing arrives unless we poll.

Framing (same in both directions), each BLE frame = [marker][len][payload...]:
    marker 0xFE  -> FIRST frame of a message
    marker 0x00..0xFE-1 -> continuation frame, value = sequence number
    marker 0xFF  -> LAST frame of a message
    len    = number of payload bytes in THIS frame
A full logical message = concat(payload of FE, 00, 01, ... , FF).

This script:
  * scans by NAME so it works on both BlueZ (Linux, real MAC) and CoreBluetooth
    (macOS, UUID) without editing addresses,
  * subscribes to notifications and reassembles FE..FF into whole messages,
  * stamps every event with absolute time + delta-since-last so the polling
    cadence is directly visible,
  * cycles a configurable list of poll commands at a fixed interval,
  * logs everything to artifacts/poll_<timestamp>.log for offline analysis.

Usage examples:
    python3 treadmill_poll.py                      # default: bare 4-byte polls @100ms
    python3 treadmill_poll.py --interval 0.05      # faster clock
    python3 treadmill_poll.py --no-init            # skip handshake, poll only
    python3 treadmill_poll.py --duration 60
"""

import argparse
import asyncio
import time
from datetime import datetime

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "I_TL"

# Service 00001533-… (the iFit/Nordic proprietary control service)
NOTIFY_CHAR = "00001535-1412-efde-1523-785feabcd123"  # handle 0x000B, NOTIFY/READ
WRITE_CHAR = "00001534-1412-efde-1523-785feabcd123"   # handle 0x000E, WRITE/READ

# ---- Command sets -----------------------------------------------------------
# Full poll triplet GROUPS (qdomyos-zwift nordictrack10 noOp frames). Bare 4-byte
# headers only return a 6-byte ack -- the device streams telemetry pages only when
# it receives the complete FE / 00 / FF group. Each group's 3 frames are written
# back-to-back, then we sleep one interval before the next group.
# The FE021403 group is what the real iFit app sent to get the live speed page
# 0x2F (the treadmill answers with header FE023304 + a 0012 0104 02 2F frame
# carrying speed at bytes 10,11). Exact frames lifted from our own capture
# (treadmill_att.txt); frame 2's last 4 bytes were truncated in the log -> 00 pad.
POLL_GROUPS = [
    [  # page 0x14 request -> yields live page 0x2F (speed/incline)
        bytes.fromhex("FE021403"),
        bytes.fromhex("001202040210041002000A1394330010" + "00000000"),
        bytes.fromhex("FF0218F2" + "00" * 16),
    ],
]

# Full nordictrack10 init handshake (complete 20-byte frames recovered from
# qdomyos-zwift btinit() -- these match our truncated capture exactly). Replay
# verbatim; the 8187/8088/8890 bytes were never decoded but replay works.
_INIT_HEX = [
    "FE020802",
    "FF08020402040204818700000000000000000000",
    "FE020802",
    "FF08020402040404808800000000000000000000",
    "FE020802",
    "FF08020402040404889000000000000000000000",
    "FE020A02",
    "FF0A0204020602068200008A0000000000000000",
    "FE020A02",
    "FF0A0204020602068400008C0000000000000000",
    "FE020802",
    "FF08020402040204959B00000000000000000000",
    "FE022C04",
    "001202040228042890040061D85DD051D055E861",
    "0112F88D009120D548E1983DD07110B548E1B84D",
    "FF08E0B140800200007500000000000000000000",
]
INIT_COMMANDS = [bytes.fromhex(h) for h in _INIT_HEX]

T0 = time.monotonic()
_last_evt = {"t": None}
_logfile = None


def _ts():
    return time.monotonic() - T0


def log(line: str):
    stamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
    out = f"[{stamp} +{_ts():7.3f}s] {line}"
    print(out, flush=True)
    if _logfile:
        _logfile.write(out + "\n")
        _logfile.flush()


def decode_telemetry(b: bytes):
    """Decode a state notification (per qdomyos-zwift proformtreadmill parser).

    Valid frame: 20 bytes starting 00 12 01 04 02, byte5 in {0x31,0x34} for our
    branch. Returns dict or None. Speed/incline little-endian /100; watts raw.
    """
    # Accept ANY 0012 0104 02 page during testing so we can spot which page byte5
    # carries live speed (QZ uses byte5 in {0x31,0x34} for a running T6.5S, but the
    # page id may differ on our fw -- watch which one changes when you walk).
    if len(b) != 20 or b[0] != 0x00 or b[1] != 0x12 or b[2] != 0x01 or b[3] != 0x04 or b[4] != 0x02:
        return None
    if all(x == 0xFF for x in b[12:20]):
        return None
    speed = ((b[11] << 8) | b[10]) / 100.0
    incl_raw = (b[13] << 8) | b[12]
    incline = (incl_raw - 0x10000 if incl_raw & 0x8000 else incl_raw) / 100.0
    watts = (b[15] << 8) | b[14]
    return {"page": b[5], "speed": speed, "incline": incline, "watts": watts}


def frame_kind(marker: int) -> str:
    if marker == 0xFE:
        return "FIRST"
    if marker == 0xFF:
        return "LAST"
    return f"CONT#{marker}"


class Reassembler:
    """Collect FE..FF frames into whole messages."""

    def __init__(self):
        self.buf = bytearray()
        self.collecting = False
        self.frames = 0

    def feed(self, data: bytes):
        """Return a completed message (bytes) or None."""
        if not data:
            return None
        marker = data[0]
        if marker == 0xFE:
            self.buf = bytearray()
            self.collecting = True
            self.frames = 0
        if not self.collecting:
            return None
        # payload = bytes after [marker][len]; trust len if sane
        length = data[1] if len(data) > 1 else 0
        payload = data[2:2 + length] if length and 2 + length <= len(data) else data[2:]
        self.buf += payload
        self.frames += 1
        if marker == 0xFF:
            msg = bytes(self.buf)
            self.collecting = False
            n = self.frames
            self.frames = 0
            return (msg, n)
        return None


def notify_handler(reasm: Reassembler):
    def handler(_sender, data: bytearray):
        b = bytes(data)
        now = time.monotonic()
        delta = "" if _last_evt["t"] is None else f" Δ{(now - _last_evt['t']) * 1000:6.1f}ms"
        _last_evt["t"] = now
        marker = b[0] if b else 0
        log(f"  RX {frame_kind(marker):7} {b.hex()}{delta}")
        tel = decode_telemetry(b)
        if tel:
            log(f"     *** PAGE 0x{tel['page']:02x}  speed={tel['speed']:.2f}  "
                f"incline={tel['incline']:.2f}  watts={tel['watts']}")
        done = reasm.feed(b)
        if done:
            msg, nframes = done
            ascii_view = "".join(chr(c) if 32 <= c < 127 else "." for c in msg)
            log(f"  >> MESSAGE ({nframes} frames, {len(msg)}B): {msg.hex()}")
            log(f"     ascii: {ascii_view}")
    return handler


async def find_device(name, address, timeout):
    if address:
        log(f"Looking up {address} ...")
        dev = await BleakScanner.find_device_by_address(address, timeout=timeout)
        if dev:
            return dev
    log(f"Scanning for name '{name}' ({timeout:.0f}s) ...")
    return await BleakScanner.find_device_by_name(name, timeout=timeout)


async def run(args):
    dev = await find_device(args.name, args.address, args.scan_timeout)
    if not dev:
        log("Device not found. Power on the treadmill and make sure no other "
            "app (iFit) is connected.")
        return
    log(f"Found {dev.name} @ {dev.address}")

    reasm = Reassembler()
    async with BleakClient(dev) as client:
        log(f"Connected: {client.is_connected}")
        await client.start_notify(NOTIFY_CHAR, notify_handler(reasm))
        log("Subscribed to notifications.")

        if not args.no_init:
            log("--- INIT handshake ---")
            for cmd in INIT_COMMANDS:
                log(f"TX init {cmd.hex()}")
                try:
                    await client.write_gatt_char(WRITE_CHAR, cmd, response=True)
                except Exception as e:
                    log(f"  init write failed: {e}")
                await asyncio.sleep(args.interval)

        log(f"--- POLL loop: {len(POLL_GROUPS)} groups @ {args.interval*1000:.0f}ms, "
            f"{args.duration}s --- (walk on the belt to see speed change)")
        end = time.monotonic() + args.duration
        i = 0
        while time.monotonic() < end:
            group = POLL_GROUPS[i % len(POLL_GROUPS)]
            i += 1
            log(f"TX poll group {group[0].hex()} (+{len(group)-1} frames)")
            for frame in group:
                try:
                    await client.write_gatt_char(WRITE_CHAR, frame, response=True)
                except Exception as e:
                    log(f"  poll write failed: {e}")
            await asyncio.sleep(args.interval)

        await client.stop_notify(NOTIFY_CHAR)
        log("Done.")


def main():
    global _logfile
    p = argparse.ArgumentParser(description="Instrumented iFit treadmill BLE poller")
    p.add_argument("--name", default=DEVICE_NAME)
    p.add_argument("--address", default=None, help="MAC (Linux) or UUID (macOS); else scan by name")
    p.add_argument("--interval", type=float, default=0.10, help="seconds between writes")
    p.add_argument("--duration", type=float, default=30.0, help="poll loop seconds")
    p.add_argument("--scan-timeout", type=float, default=10.0)
    p.add_argument("--no-init", action="store_true", help="skip handshake, poll only")
    args = p.parse_args()

    fname = f"poll_{datetime.now().strftime('%Y%m%d_%H%M%S')}.log"
    _logfile = open(fname, "w")
    log(f"Logging to {fname}")
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        log("Interrupted.")
    finally:
        _logfile.close()


if __name__ == "__main__":
    main()
