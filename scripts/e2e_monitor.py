#!/usr/bin/env python3
"""e2e_monitor.py — end-to-end press->ping monitor for the SP-1 beacon firmware.

Watches the SP-1's USB-CDC console and prints one clean line per event in the
button -> nRF -> CYW20706 -> nRF loop:

  - a button edge          ("BTN T1 down")
  - the PING going out     ("BT: tx grp=f0 code=03")
  - the module's reply     ("BT: rx wiced grp=f0 ..." — the app's READY event),
    with the press->reply round-trip time

Usage:
  scripts/e2e_monitor.py               # auto-detect /dev/cu.usbmodem*
  scripts/e2e_monitor.py --port PORT   # explicit port
  scripts/e2e_monitor.py --all         # also pass through every other line

Needs pyserial:  pip3 install pyserial
"""
import argparse
import glob
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is required:  pip3 install pyserial")

RE_BTN = re.compile(r"^BTN (\S+) (down|up)")
RE_TX_PING = re.compile(r"^BT: tx grp=f0 code=03")
RE_RX_FELDD = re.compile(r"^BT: rx wiced grp=f0 code=(\S+) len=\d+:(.*)")
RE_MODULE = re.compile(r"^BT: (module .*)")


def find_port() -> str:
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("no /dev/cu.usbmodem* found — is the SP-1 plugged in and flashed?")
    if len(ports) > 1:
        print(f"multiple ports, using {ports[0]} (others: {', '.join(ports[1:])})")
    return ports[0]


def stamp() -> str:
    return time.strftime("%H:%M:%S", time.localtime()) + f".{int(time.time() * 1000) % 1000:03d}"


def main() -> None:
    ap = argparse.ArgumentParser(description="SP-1 press->ping E2E monitor")
    ap.add_argument("--port", help="serial port (default: first /dev/cu.usbmodem*)")
    ap.add_argument("--all", action="store_true", help="pass through every console line")
    args = ap.parse_args()

    port = args.port or find_port()
    ser = serial.Serial(port, 115200, timeout=0.2)
    print(f"listening on {port} — press SP-1 buttons (ctrl-c to quit)")

    t_press = None      # last button-down, for press->reply RTT
    t_ping = None       # last ping tx, for ping->reply RTT
    last_btn = "?"
    pings = replies = 0

    try:
        while True:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").strip()
            if not line:
                continue
            now = time.monotonic()

            m = RE_BTN.match(line)
            if m:
                name, edge = m.groups()
                if edge == "down":
                    t_press, last_btn = now, name
                print(f"{stamp()}  button {name} {edge}")
                continue

            if RE_TX_PING.match(line):
                t_ping = now
                pings += 1
                print(f"{stamp()}    -> PING (#{pings})")
                continue

            m = RE_RX_FELDD.match(line)
            if m:
                code, payload = m.groups()
                replies += 1
                rtt = f" — answered in {(now - t_ping) * 1000:.0f} ms" if t_ping else ""
                press = f", {(now - t_press) * 1000:.0f} ms after {last_btn} press" if t_press else ""
                print(f"{stamp()}    <- module event code={code}{rtt}{press}  [{payload.strip()}]")
                t_ping = t_press = None
                continue

            m = RE_MODULE.match(line)
            if m:
                print(f"{stamp()}  {m.group(1)}")
                continue

            if args.all:
                print(f"{stamp()}  | {line}")
    except KeyboardInterrupt:
        print(f"\n{pings} pings sent, {replies} module replies seen")


if __name__ == "__main__":
    main()
