#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# ///
"""gen_blobs.py — turn the CYW20706 minidriver hex + the beacon module DS image
into firmware/app/src/cybt_blobs.h for the M4 flasher (CONFIG_SP1_BT_DOWNLOAD).

Both inputs are Intel HEX. The minidriver is Infineon-licensed and our DS image
is Cypress-derived, so the generated header is GITIGNORED — never commit it, and
never commit these hex inputs into this repo.

  minidriver:  ~/src/home-auto/sp-1-ble-radio/mtb_shared/wiced_btsdk/dev-kit/bsp/
               TARGET_CYBT-353027-EVAL/release-*/uart.hex
  DS image:    ~/src/home-auto/sp-1-ble-radio/LE_Hello_Sensor/build/CYBT-353027-EVAL/
               Debug/BLE_HelloSensor_download.hex

Usage:
  scripts/gen_blobs.py --minidriver <uart.hex> --ds <..._download.hex> \
      [--ds-base 0xFF003000] [--ss-template <flash_dump.bin>] \
      [--out firmware/app/src/cybt_blobs.h]

--ss-template takes a raw full-flash dump (fw.sh monitordump output) from a
KNOWN-GOOD SP-1 and emits its first SS_TEMPLATE_LEN Static-Section bytes as the
provisioning identity template, with the 6 per-unit BD_ADDR bytes (the type-0x40
record payload at offset 21) zeroed. The on-device provisioner requires a
candidate radio's SS to match this template byte-for-byte outside the BD_ADDR —
feldd's template-equality gate — so it can refuse any unit whose SS layout
deviates, and never mis-target a write. The template contains no per-unit
identity (BD_ADDR masked) and no vendor code. Required for CONFIG_SP1_PROVISION.

Safety checks (refuse to emit on any failure):
  - the minidriver's start-linear-address matches CYBT_MINIDRIVER_RAM (0x220000)
  - EVERY DS record sits at/above --ds-base and below end-of-flash (0xFF080000)
  - NO record lies below the DS base (would touch VS/SS) — the SS record that a
    fresh build carries is dropped and its presence is reported
  - the SS template must itself parse: TLV walk finds the type-0x02 DS record
    pointing at --ds-base, and the BD_ADDR record type-0x40 sits at offset 18
"""
import argparse
import sys

MINIDRIVER_RAM = 0x00220000
FLASH_BASE = 0xFF000000
FLASH_END = 0xFF080000

SS_TEMPLATE_LEN = 64       # matches the flasher's SS_LEN read window
SS_BDADDR_OFF = 21         # type-0x40 record payload: the 6 per-unit BD_ADDR bytes
SS_BDADDR_LEN = 6


def build_ss_template(path, ds_base):
    """Read the SS from a raw flash dump, sanity-parse it, mask the BD_ADDR."""
    with open(path, "rb") as f:
        ss = bytearray(f.read(SS_TEMPLATE_LEN))
    if len(ss) < SS_TEMPLATE_LEN:
        sys.exit(f"{path}: shorter than {SS_TEMPLATE_LEN} bytes — not a flash dump")
    # Sanity 1: the BD_ADDR record (type 0x40, len 6) sits where we mask.
    if ss[18] != 0x40 or ss[19] != 0x06 or ss[20] != 0x00:
        sys.exit(f"{path}: no type-0x40 len-6 BD_ADDR record at SS offset 18 — "
                 "unexpected SS layout, refusing to emit a template")
    # Sanity 2: the TLV walk finds a type-0x02 record whose DS base == ds_base.
    off, found = 0, None
    while off + 3 <= len(ss):
        t, rlen = ss[off], ss[off + 1] | (ss[off + 2] << 8)
        if t == 0xFF or off + 3 + rlen > len(ss):
            break
        if t == 0x02 and rlen >= 4:
            v = int.from_bytes(ss[off + 3:off + 7], "little")
            found = v + (FLASH_BASE if v < FLASH_BASE else 0)
            break
        off += 3 + rlen
    if found != ds_base:
        sys.exit(f"{path}: SS type-0x02 DS base "
                 f"{'0x%08X' % found if found else 'not found'} != 0x{ds_base:08X}")
    for i in range(SS_BDADDR_OFF, SS_BDADDR_OFF + SS_BDADDR_LEN):
        ss[i] = 0x00       # mask the per-unit identity out of the template
    return bytes(ss)


def parse_ihex(path):
    """Return (segments, start_linear_addr). segments = sorted [(addr, bytes)],
    merged into contiguous runs."""
    ext = 0                # upper 16 bits from type-04 records
    start_linear = None
    chunks = {}            # addr -> byte
    with open(path) as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or not line.startswith(":"):
                continue
            b = bytes.fromhex(line[1:])
            n, a_hi, a_lo, rectype = b[0], b[1], b[2], b[3]
            data = b[4:4 + n]
            if (sum(b) & 0xFF) != 0:
                sys.exit(f"{path}:{lineno}: bad checksum")
            if rectype == 0x00:        # data
                addr = (ext << 16) + (a_hi << 8) + a_lo
                for i, byte in enumerate(data):
                    chunks[addr + i] = byte
            elif rectype == 0x01:      # EOF
                break
            elif rectype == 0x04:      # extended linear address
                ext = (data[0] << 8) + data[1]
            elif rectype == 0x05:      # start linear address
                start_linear = int.from_bytes(data, "big")
            elif rectype == 0x02:      # extended segment address
                ext = ((data[0] << 8) + data[1]) >> 12
            # other record types ignored
    # merge contiguous
    segments = []
    for addr in sorted(chunks):
        if segments and addr == segments[-1][0] + len(segments[-1][1]):
            segments[-1] = (segments[-1][0], segments[-1][1] + bytes([chunks[addr]]))
        else:
            segments.append((addr, bytes([chunks[addr]])))
    return segments, start_linear


def c_array(name, data):
    lines = [f"static const uint8_t {name}[] = {{"]
    for i in range(0, len(data), 12):
        row = ", ".join(f"0x{x:02X}" for x in data[i:i + 12])
        lines.append(f"    {row},")
    lines.append("};")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--minidriver", required=True)
    ap.add_argument("--ds", required=True)
    ap.add_argument("--ds-base", default="0xFF003000")
    ap.add_argument("--ss-template", help="raw flash dump of a known-good unit; "
                    "emits the BD_ADDR-masked SS identity template")
    ap.add_argument("--out", default="firmware/app/src/cybt_blobs.h")
    args = ap.parse_args()
    ds_base = int(args.ds_base, 0)

    ss_template = None
    if args.ss_template:
        ss_template = build_ss_template(args.ss_template, ds_base)
        print(f"SS template: {len(ss_template)} bytes, BD_ADDR "
              f"[{SS_BDADDR_OFF}..{SS_BDADDR_OFF + SS_BDADDR_LEN}) masked")

    md_segs, md_start = parse_ihex(args.minidriver)
    base = md_segs[0][0]
    # The load address comes from the hex itself (doc: the type-05 start-linear-
    # address record; else the lowest data segment). Do NOT hardcode it — it is
    # SDK/part specific. Report it loudly so it can be cross-checked on the bench.
    if md_start is None:
        md_start = base
        print(f"note: minidriver has no start-linear-address record; "
              f"launching at lowest segment 0x{md_start:08X}")
    # The current CYBT_353027_EVAL minidriver.hex (Programming Tools; identical
    # to the BSP's uart.hex) loads at 0x0D0200 — the doc's 0x00220000 is from an
    # older SDK. §7 is explicit that the address comes from the type-05 record,
    # which is what we use, so 0x0D0200 here is CORRECT, not a warning.
    if md_start not in (MINIDRIVER_RAM, 0x000D0201, 0x000D0200):
        print(f"NOTE: minidriver load/launch addr 0x{md_start:08X} is neither the "
              f"doc's 0x{MINIDRIVER_RAM:08X} nor the known 353027 value 0x000D0200. "
              f"Confirm you have the CYBT_353027_EVAL minidriver.hex.")
    md = bytearray()
    for a, d in md_segs:
        md.extend(b"\xff" * (a - (base + len(md))))
        md.extend(d)
    print(f"minidriver: {len(md)} bytes, launch @ 0x{md_start:08X}")

    ds_segs, _ = parse_ihex(args.ds)
    # Partition DS records: keep those in [ds_base, FLASH_END); report/drop the
    # rest (the SS record a fresh build carries lives at FLASH_BASE).
    kept, dropped = [], []
    for a, d in ds_segs:
        if ds_base <= a < FLASH_END and a + len(d) <= FLASH_END:
            kept.append((a, d))
        else:
            dropped.append((a, len(d)))
    for a, n in dropped:
        tag = " (SS record — correctly dropped)" if a < ds_base else ""
        print(f"dropping DS record 0x{a:08X} ({n} bytes){tag}")
    if not kept:
        sys.exit("no DS records at/above the DS base — wrong --ds-base or image?")
    lo = min(a for a, _ in kept)
    hi = max(a + len(d) for a, d in kept)
    if lo != ds_base:
        print(f"warning: lowest kept DS record 0x{lo:08X} != --ds-base "
              f"0x{ds_base:08X} — the app may be built for a different DS base")

    # Flatten kept records into one blob from ds_base, 0xFF-filling gaps.
    ds = bytearray()
    for a, d in sorted(kept):
        ds.extend(b"\xff" * (a - (ds_base + len(ds))))
        ds.extend(d)
    print(f"DS image: {len(ds)} bytes @ 0x{ds_base:08X}..0x{ds_base + len(ds):08X}")

    with open(args.out, "w") as f:
        f.write("/* GENERATED by scripts/gen_blobs.py — DO NOT COMMIT.\n"
                " * Vendor-derived (Infineon minidriver + Cypress-derived DS).\n"
                " * Regenerate after any module-app rebuild. */\n")
        f.write("#ifndef CYBT_BLOBS_H\n#define CYBT_BLOBS_H\n#include <stdint.h>\n\n")
        # Write base = lowest segment; launch = start-linear-address (may have
        # bit0 set for a Thumb entry, so it can differ from the write base).
        f.write(f"#define CYBT_BLOB_MINIDRIVER_ADDR   0x{int(base):08X}u\n")
        f.write(f"#define CYBT_BLOB_MINIDRIVER_LAUNCH 0x{int(md_start):08X}u\n")
        f.write(c_array("cybt_minidriver", md) + "\n\n")
        f.write(f"#define CYBT_BLOB_DS_ADDR 0x{ds_base:08X}u\n")
        f.write(c_array("cybt_ds_image", ds) + "\n\n")
        if ss_template is not None:
            f.write("/* SS identity template from a known-good unit's dump; the 6\n"
                    " * per-unit BD_ADDR bytes are zeroed (masked in the compare). */\n")
            f.write("#define CYBT_HAVE_SS_TEMPLATE 1\n")
            f.write(c_array("cybt_ss_template", ss_template) + "\n\n")
        f.write("#endif /* CYBT_BLOBS_H */\n")
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
