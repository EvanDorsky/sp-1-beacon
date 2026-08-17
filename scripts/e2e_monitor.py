#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["bleak"]
# ///
"""e2e_monitor.py — over-the-air E2E monitor for the SP-1 beacon firmware.

Scans for the SP-1's BLE advertisements (the CYW20706 module's broadcast) and
prints one line per advertising BURST, so the whole chain is proven end to end:

  button press -> nRF -> WICED-HCI -> module -> RADIO -> this machine

Press any SP-1 button except Vol-/RWD and a ~2 s burst should appear here.
No pairing needed — advertising packets are broadcast; any listener sees them.

The module (still running feldd's BLE app) advertises as "feldd": an HID
service (0x1812) in the primary AD and the BLE-MIDI service + name in the scan
response. We match on the name and/or the distinctive BLE-MIDI UUID.

Usage:
  uv run scripts/e2e_monitor.py          # uv installs bleak automatically
  uv run scripts/e2e_monitor.py --any    # print every BLE sighting (debug)
  scripts/e2e_monitor.py --name NAME     # plain python3 (needs: pip3 install bleak)

macOS: grant your terminal Bluetooth permission (System Settings > Privacy).
"""
import argparse
import asyncio
import sys
import time

try:
    from bleak import BleakScanner
except ImportError:
    sys.exit("bleak is required:  pip3 install bleak")

BLE_MIDI_UUID = "03b80e5a-ede8-4b33-a751-6ce34ec4c700"

# The sp1-beacon module app (M3b+) broadcasts manufacturer data instead:
# company 0xFFFF (SIG internal-use), then the 9-byte beacon_state payload
# (version, seq, buttons u16 LE, 4x fader u8, battery). Decode it live.
SP1_COMPANY_ID = 0xFFFF
SP1_STATE_VER = 1
BTN_NAMES = ["PLAY", "T1", "T2", "T3", "T4", "VOL+", "VOL-", "FWD", "RWD"]


def decode_state(mfr: dict):
    payload = mfr.get(SP1_COMPANY_ID)
    if not payload or len(payload) < 9 or payload[0] != SP1_STATE_VER:
        return None
    buttons = payload[2] | (payload[3] << 8)
    held = [BTN_NAMES[i] for i in range(9) if buttons & (1 << i)] or ["-"]
    batt = payload[8]
    return (f"seq={payload[1]} btn={'+'.join(held)} "
            f"faders={payload[4]}/{payload[5]}/{payload[6]}/{payload[7]} "
            f"batt={'?' if batt == 0xFF else batt}")

# A gap this long with no sighting ends the burst. The firmware's burst is ~2 s
# of advertising at a fast interval, so intra-burst gaps stay well under this.
BURST_GAP_S = 1.5


def stamp() -> str:
    return time.strftime("%H:%M:%S", time.localtime()) + f".{int(time.time() * 1000) % 1000:03d}"


class BurstWatch:
    def __init__(self, name: str, show_any: bool):
        self.name = name.lower()
        self.show_any = show_any
        self.burst_start = None     # monotonic time of first sighting
        self.last_seen = None
        self.sightings = 0
        self.last_rssi = None
        self.bursts = 0
        self.last_state = None      # decoded sp1 state, printed on change

    def matches(self, device, adv) -> bool:
        if decode_state(adv.manufacturer_data or {}) is not None:
            return True                       # sp1-beacon state broadcast
        local = (adv.local_name or device.name or "").lower()
        if local == self.name:
            return True                       # feldd-era presence beacon
        return any(u.lower() == BLE_MIDI_UUID for u in (adv.service_uuids or []))

    def on_detect(self, device, adv) -> None:
        now = time.monotonic()
        if self.show_any:
            print(f"{stamp()}  . {adv.local_name or device.name or '?':24s} "
                  f"rssi={adv.rssi:4d}  uuids={adv.service_uuids or []}")
        if not self.matches(device, adv):
            return
        self.last_rssi = adv.rssi
        state = decode_state(adv.manufacturer_data or {})
        if self.burst_start is None:
            self.bursts += 1
            self.sightings = 0
            self.burst_start = now
            what = state or f"name={adv.local_name or device.name!r}"
            print(f"{stamp()}  BURST #{self.bursts} on the air  {what} rssi={adv.rssi}")
        elif state and state != self.last_state:
            print(f"{stamp()}    {state} rssi={adv.rssi}")
        self.last_state = state
        self.sightings += 1
        self.last_seen = now

    def check_burst_end(self) -> None:
        if self.burst_start is None or self.last_seen is None:
            return
        now = time.monotonic()
        if now - self.last_seen > BURST_GAP_S:
            dur = self.last_seen - self.burst_start
            print(f"{stamp()}  burst #{self.bursts} over — {dur:.1f} s on air, "
                  f"{self.sightings} sighting(s), last rssi={self.last_rssi}")
            self.burst_start = None
            self.last_seen = None


async def main() -> None:
    ap = argparse.ArgumentParser(description="SP-1 over-the-air E2E monitor")
    ap.add_argument("--name", default="feldd", help="advertised device name (default: feldd)")
    ap.add_argument("--any", action="store_true", help="print every BLE sighting (debug)")
    args = ap.parse_args()

    watch = BurstWatch(args.name, args.any)
    print(f"scanning for '{args.name}' advertisements — press SP-1 buttons (ctrl-c to quit)")
    print("(no pairing needed; if a bonded Mac/iPhone auto-connects it may shorten a burst)")

    async with BleakScanner(watch.on_detect):
        while True:
            await asyncio.sleep(0.2)
            watch.check_burst_end()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nbye")
