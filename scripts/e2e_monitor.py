#!/usr/bin/env python3
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
  scripts/e2e_monitor.py                 # watch for "feldd" bursts
  scripts/e2e_monitor.py --name NAME     # a different advertised name
  scripts/e2e_monitor.py --any           # print every BLE sighting (debug)

Needs bleak:  pip3 install bleak
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

    def matches(self, device, adv) -> bool:
        local = (adv.local_name or device.name or "").lower()
        if local == self.name:
            return True
        return any(u.lower() == BLE_MIDI_UUID for u in (adv.service_uuids or []))

    def on_detect(self, device, adv) -> None:
        now = time.monotonic()
        if self.show_any:
            print(f"{stamp()}  . {adv.local_name or device.name or '?':24s} "
                  f"rssi={adv.rssi:4d}  uuids={adv.service_uuids or []}")
        if not self.matches(device, adv):
            return
        self.last_rssi = adv.rssi
        if self.burst_start is None:
            self.bursts += 1
            self.sightings = 0
            self.burst_start = now
            print(f"{stamp()}  BURST #{self.bursts} on the air  "
                  f"name={adv.local_name or device.name!r} rssi={adv.rssi}")
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
