#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["bleak"]
# ///
"""e2e_monitor.py — over-the-air E2E monitor for the SP-1 BTHome beacon.

Scans for the SP-1's BTHome v2 advertisements (the CYW20706 module's Service
Data under UUID 0xFCD2 — the same bytes Home Assistant decodes) and prints one
line per advertising BURST plus each button EVENT, proving the chain:

  button press -> nRF -> WICED-HCI -> module -> RADIO -> this machine

Press any SP-1 button and a burst with a press/long_press event should appear.
No pairing needed — advertising packets are broadcast; any listener sees them.
Wire format: bluetooth/broadcast-format.md (devinfo + pid + battery + 9
positional button-event objects).

Usage:
  uv run scripts/e2e_monitor.py          # uv installs bleak automatically
  uv run scripts/e2e_monitor.py --any    # print every BLE sighting (debug)

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

# BTHome v2 service data UUID (bleak keys service_data by the 128-bit form).
BTHOME_UUID_128 = "0000fcd2-0000-1000-8000-00805f9b34fb"

BTN_NAMES = ["PLAY", "T1", "T2", "T3", "T4", "VOL+", "VOL-", "FWD", "RWD"]
EV_NAMES = {0x01: "press", 0x02: "double", 0x03: "triple",
            0x04: "long_press", 0x05: "long_double", 0x06: "long_triple",
            0x80: "hold"}


def decode_state(svc: dict):
    """Decode a BTHome v2 payload -> summary string, or None if not the SP-1."""
    payload = svc.get(BTHOME_UUID_128)
    if not payload or len(payload) < 3:
        return None
    if (payload[0] >> 5) != 2:            # BTHome version bits
        return None
    pid = batt = None
    events = []
    faders = []
    i, btn = 1, 0
    while i + 1 < len(payload) + 1 and i < len(payload):
        obj = payload[i]
        if obj == 0x00 and i + 1 < len(payload):        # packet id
            pid = payload[i + 1]; i += 2
        elif obj == 0x01 and i + 1 < len(payload):      # battery %
            batt = payload[i + 1]; i += 2
        elif obj == 0x09 and i + 1 < len(payload):      # count u8: a fader value
            faders.append(payload[i + 1]); i += 2
        elif obj == 0x3A and i + 1 < len(payload):      # button event
            ev = payload[i + 1]
            if ev != 0x00:
                name = BTN_NAMES[btn] if btn < len(BTN_NAMES) else f"btn{btn + 1}"
                events.append(f"{name}:{EV_NAMES.get(ev, hex(ev))}")
            btn += 1; i += 2
        else:
            break                                        # unknown object: stop
    body = (f"faders={'/'.join(map(str, faders))}" if faders
            else f"ev={'+'.join(events) if events else '-'}")
    return f"pid={pid} {body} batt={'?' if batt is None else batt}"

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
        if decode_state(adv.service_data or {}) is not None:
            return True                       # sp1 BTHome broadcast
        local = (adv.local_name or device.name or "").lower()
        return local == self.name             # legacy fallback (pre-BTHome builds)

    def on_detect(self, device, adv) -> None:
        now = time.monotonic()
        if self.show_any:
            print(f"{stamp()}  . {adv.local_name or device.name or '?':24s} "
                  f"rssi={adv.rssi:4d}  uuids={adv.service_uuids or []}")
        if not self.matches(device, adv):
            return
        self.last_rssi = adv.rssi
        state = decode_state(adv.service_data or {})
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
