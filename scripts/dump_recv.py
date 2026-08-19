#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial"]
# ///
"""dump_recv.py — receive a CYW20706 flash dump from the SP-1 over USB-CDC.

Pairs with the CONFIG_SP1_BT_DUMP firmware (bt_dump.c). Attaches to the serial
port, echoes the setup lines, then on `DUMPSTART <base> <len>` reads exactly
<len> raw bytes (length-delimited — a marker byte inside the flash can't
false-trigger), then reads `DUMPEND crc32=<hex>` and compares the nRF's CRC-32
to one it computes locally. Writes the dump to a .bin on match.

Usage:
  ./scripts/fw.sh monitordump [out.bin]        # normal
  uv run scripts/dump_recv.py -o out.bin [--port PORT] [--no-go]

Flow: start this FIRST (it waits for the port + sends a GO byte), then flash the
dump build (./scripts/fw.sh dump && ./scripts/fw.sh flash) or reset the SP-1.

Run it twice and diff the two .bin files for extra assurance (flash reads are
deterministic, so two clean dumps are byte-identical).
"""
import argparse
import glob
import re
import sys
import time
import zlib

try:
    import serial
except ImportError:
    sys.exit("pyserial required:  pip3 install pyserial  (or use: ./scripts/fw.sh monitordump)")

START_RE = re.compile(rb"^DUMPSTART ([0-9A-Fa-f]+) (\d+)\s*$")
END_RE = re.compile(rb"^DUMPEND crc32=([0-9A-Fa-f]+)\s*$")
DATA_RE = re.compile(rb"^>([0-9A-Fa-f]+)\s*$")   # one hex line per chunk


def find_port(explicit):
    if explicit:
        return explicit
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        ports = sorted(glob.glob("/dev/cu.usbmodem*"))
        if ports:
            return ports[0]
        time.sleep(0.3)
    sys.exit("no /dev/cu.usbmodem* after 30s — flash the dump build / reset the SP-1")


def read_line(ser):
    """Read one \\n-terminated line (bytes, without the newline)."""
    buf = bytearray()
    while True:
        b = ser.read(1)
        if not b:
            return None            # timeout
        if b == b"\n":
            return bytes(buf).rstrip(b"\r")
        buf += b


def main():
    ap = argparse.ArgumentParser(description="SP-1 CYW20706 flash-dump receiver")
    ap.add_argument("-o", "--out", default="sp1_flash_dump.bin")
    ap.add_argument("--port")
    ap.add_argument("--no-go", action="store_true", help="don't send the GO byte (passive)")
    args = ap.parse_args()

    port = find_port(args.port)
    print(f"listening on {port} -> {args.out}")
    ser = serial.Serial(port, 115200, timeout=1)
    if not args.no_go:
        ser.write(b"G")            # handshake: begin the stream now

    # Phase 1: echo setup lines until DUMPSTART.
    base = length = None
    t0 = time.monotonic()
    while True:
        line = read_line(ser)
        if line is None:
            if time.monotonic() - t0 > 60:
                sys.exit("timed out waiting for DUMPSTART")
            continue
        m = START_RE.match(line)
        if m:
            base = int(m.group(1), 16)
            length = int(m.group(2))
            break
        if line.startswith(b"DUMPABORT"):
            sys.exit(f"firmware aborted the dump ({line.decode('utf-8','replace')})")
        if line:
            print(f"  | {line.decode('utf-8', 'replace')}")

    print(f"DUMPSTART base=0x{base:08X} len={length} — receiving...")

    # Phase 2: one hex line per chunk (">ABCD..."), in order, until `length` bytes.
    data = bytearray()
    nrf_crc = None
    last = time.monotonic()
    while len(data) < length:
        line = read_line(ser)
        if line is None:
            if time.monotonic() - last > 15:
                sys.exit(f"stalled at {len(data)}/{length} bytes")
            continue
        last = time.monotonic()
        m = DATA_RE.match(line)
        if m:
            data += bytes.fromhex(m.group(1).decode())
            if len(data) % 65536 < 256:
                print(f"  {len(data)}/{length} bytes ({100*len(data)//length}%)")
            continue
        if line.startswith(b"DUMPABORT"):
            partial = args.out + ".partial"
            with open(partial, "wb") as f:
                f.write(data)
            print(f"  firmware aborted at {len(data)} bytes ({line.decode('utf-8','replace')})")
            print(f"  saved partial dump -> {partial} ({len(data)} bytes)")
            print("  NOTE: this still captures SS (0x0-0x1000) + VS (0x1000-0x2000),")
            print("  the unit-unique metadata; the DS beyond it restores from feldd's image.")
            return 3
        if line:
            print(f"  | {line.decode('utf-8', 'replace')}")

    host_crc = zlib.crc32(data) & 0xFFFFFFFF

    # Phase 3: DUMPEND crc32=...
    for _ in range(40):
        line = read_line(ser)
        if line is None:
            continue
        m = END_RE.match(line)
        if m:
            nrf_crc = int(m.group(1), 16)
            break
    ser.close()

    with open(args.out, "wb") as f:
        f.write(data)
    print(f"wrote {len(data)} bytes -> {args.out}")
    print(f"host crc32 = 0x{host_crc:08X}")
    if nrf_crc is None:
        print("WARNING: no DUMPEND received — could not verify the nRF CRC")
        return 1
    print(f"nRF  crc32 = 0x{nrf_crc:08X}")
    if nrf_crc == host_crc:
        print("CRC MATCH — dump is intact.")
        return 0
    print("CRC MISMATCH. If a second dump is byte-identical to this one, it's a")
    print("CRC-algorithm mismatch (harmless); otherwise the transfer was corrupted.")
    return 2


if __name__ == "__main__":
    sys.exit(main())
