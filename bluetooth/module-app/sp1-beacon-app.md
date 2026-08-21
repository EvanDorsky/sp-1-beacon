# The sp1-beacon module app (CYW20706 / CYBT-353027-02)

This is beacon-sp-1's replacement for feldd's BLE-MIDI/HID module app: a
**pure BLE broadcaster** for the SP-1's CYW20706 radio. It has no connectable
identity at all — no pairing, no bonds, no GATT traffic on the air — it just
embeds the SP-1's control state in a non-connectable advertisement whenever
the nRF host tells it to.

Like feldd's app, it is **derived from Infineon's
`mtb-example-btsdk-ble-hello-sensor`** (target `CYBT-353027-EVAL` /
CYW20706A2, `COMPONENT_btstack_v1`, built with ModusToolbox — see the license
note in `README.md`; the modified sources live outside this repo). The
modifications are small and surgical, described here in prose so the app can
be recreated against a clean copy of the example. Status: **build-verified
with ModusToolbox 3.8** (MTB ≥ 2.1 works per the example's README);
hardware-validation lands with the M4 reflash.

## Wire protocol (the nRF host <-> module contract)

WICED-HCI over the module UART, private command group `0xF0` (opcode =
`group << 8 | code`, little-endian on the wire). Commands from the nRF:

| Code | Command | Payload | Behavior |
|------|---------|---------|----------|
| 0x03 | PING | — | reply with the READY event |
| 0x04 | ADV | 1 byte: 0/1 | stop / start non-connectable advertising |
| 0x10 | SET_STATE | 9 bytes (`beacon_state.h` format) | embed the payload as manufacturer data, (re)start advertising if stopped, ack with STATE_ACK |

Events to the nRF:

| Code | Event | Payload | When |
|------|-------|---------|------|
| 0x80 | READY | 1 byte: expected state length (9) | at boot (stack up), on transport-up, and in reply to PING |
| 0x81 | STATE_ACK | 1 byte: the applied payload's seq | after each accepted SET_STATE |

Command codes 0x03/0x04 deliberately match feldd's app (PING/ADV), so the nRF
firmware's M2-era plumbing drives either app; feldd's MIDI/HID codes are
simply not implemented, and unknown codes are ignored.

## What goes on the air

A legacy non-connectable advertisement (`BTM_BLE_ADVERT_NONCONN_HIGH`,
~100 ms interval from the default v1 cfg), no scan response, advertising with
the module's **stable factory BD_ADDR** (privacy/RPA off) so receivers can
filter on the address. AD structure:

- **Flags**: BR/EDR-not-supported only.
- **Manufacturer Specific Data**: company ID `0xFFFF` (Bluetooth SIG
  internal-use), then the 9 `beacon_state` bytes verbatim
  (version, seq, buttons u16 LE, 4× fader u8, battery pct).

`scripts/e2e_monitor.py` decodes exactly this.

## The modifications to hello_sensor, in prose

1. **`COMPONENT_btstack_v1/wiced_bt_cfg.c`** — in the non-nvram-emulation
   `transport_cfg`, set `p_status_handler` and `p_data_handler` to the app's
   transport-status / RX functions (the example ships them NULL). Set
   `.ble_advert_cfg.high_duty_nonconn_duration = 0` (infinite): the nRF host
   owns advertising lifetime; without this the stack silently stops
   advertising after 30 s.
2. **`hello_sensor.c`** — add one self-contained block: the 0xF0 opcode
   defines, a static `[company LE16][9-byte state]` manufacturer-data buffer,
   an adv builder (Flags + manufacturer data via
   `wiced_bt_ble_set_raw_advertisement_data`), start/stop via
   `wiced_bt_start_advertisements(BTM_BLE_ADVERT_NONCONN_HIGH / _OFF)`, the
   RX handler (frame layout `[opcode LE16][len LE16][payload]`; **free the
   buffer** with `wiced_transport_free_buffer` in HCI mode), READY via
   `wiced_transport_send_data`.
3. **`hello_sensor_application_init`** — do NOT: configure the eval-board
   button interrupt (unknown wiring inside the SP-1's module), start the
   example's periodic notify/battery timers, enable privacy/RPA (stable
   BD_ADDR wanted), allow pairing (`wiced_bt_set_pairable_mode(FALSE, 0)`),
   or start advertising at boot. DO: send READY. Set
   `flag_stay_connected = 0`.
4. **`hello_sensor_advertisement_stopped`** — if the host still wants
   advertising, restart `NONCONN_HIGH` (covers stack-side stops); the
   example's reconnect logic is gone.
5. **Left alone**: the GATT database (invisible under non-connectable
   advertising and kept so the v1 GATT init path stays byte-identical to the
   proven example), trace routing, and the LED helpers (never triggered —
   no button, no bonding).

## Flash safety: the app contains zero flash-write instructions

The one unrecoverable act on this module is damaging the Static Section of
its serial flash (`README.md`). This app is made **structurally incapable of
writing the flash at all**, not merely unlikely to:

- All five of the example's `wiced_hal_write_nvram` call sites are removed
  (host-info save, GATT-write save, paired-keys save, local-identity-keys
  save, and the btstack_v1 GATT handler's save). Four were unreachable anyway
  (they need a connection or a bond, and a non-connectable broadcaster has
  neither); the fifth — `BTM_LOCAL_IDENTITY_KEYS_UPDATE_EVT` — **does fire at
  boot** (the stack generates identity keys unprompted), so it was live code:
  now the keys are simply regenerated each boot, which a broadcaster never
  notices. NVRAM *reads* remain (reads are harmless).
- **Audit gate** (same idea as feldd's ELF-grep gate): after every build,
  `arm-none-eabi-nm` over all app object files must show **no references** to
  `write_nvram` / `delete_nvram` / `sflash_write` / `sflash_erase` /
  `eflash`. Verified on the current build.
- The app cannot enter download mode by itself: there is no app-side API —
  download mode is a hardware strap (CTS low at reset release), owned by the
  nRF (`findings.md`).
- A crashed or misbehaving *app* is always recoverable via Recovery-Reset
  into the mask-ROM download mode; only the flasher (M4), never this app,
  decides what gets written.

## Address check (the feldd docs' "fixed-address override")

Verified on the built image: the app source never calls
`wiced_bt_set_local_bdaddr` (the symbol exists only in the ROM map), the
generated `.cgs` carries no BD_ADDR record, and the makefile's
`BT_DEVICE_ADDRESS?=default` only feeds Infineon's own ChipLoad programming
step, which our flow does not use. With privacy/RPA disabled, the app
advertises with the module's factory Static-Section BD_ADDR — the stable
address receivers filter on.

## The flasher (M4) — status and how to run it

The nRF-side flasher is written and **build-verified**, host-tested where it
counts, but **not hardware-validated**. Layout:

- `firmware/app/src/cybt_dl.{c,h}` — the pure protocol/planning core: command
  builders, ack matching, SS TLV → DS-base parsing, and the DS-window floor
  guard. Host-tested in `firmware/test/test_cybt_dl.c` (byte-exact against the
  opcodes; the floor/identity guards hammered).
- `firmware/app/src/bt_download.c` — the dev-only I/O shell (poll-mode uart0,
  the download strap, sequencing). Compiled only under `CONFIG_SP1_BT_DOWNLOAD`.
- `scripts/gen_blobs.py` — Intel-HEX → `firmware/app/src/cybt_blobs.h`
  (gitignored): the minidriver + the DS image, SS record dropped.

Build/run (needs `cybt_blobs.h` generated first):

```
scripts/gen_blobs.py --minidriver <uart.hex> --ds <..._download.hex> --ds-base 0x...
./scripts/fw.sh dl       # DRY-RUN: enter download mode, identity gate, DS plan — NO write
./scripts/fw.sh dlarm    # ARMED: performs the DS-only write + read-back verify
```

Safety properties, verified this build:
- **No chip-erase exists** in the subsystem (`cybt_dl.h` `#error`s if
  `CYBT_DL_ALLOW_CHIP_ERASE` is defined; no 0xFFCE builder).
- **Floor guard**: every DS chunk is checked against `CYBT_DS_FLOOR` before it
  is emitted; a write toward VS/SS is refused (host-tested).
- **Identity gate before the minidriver**: the SS is read twice (must be
  byte-identical) and its DS base must equal the compile-time `CYBT_DS_BASE`,
  or the run aborts before any write machinery loads.
- The dry-run and a wrong-base blob both abort cleanly — demonstrated: with a
  blob built for the eval base while `CYBT_DS_BASE` was the SP-1's, `ds_plan()`
  refuses and the compiler proved the entire write path dead (the DS blob was
  even dropped from the image).

**Bench-open questions:**
1. **Minidriver identity + launch address — RESOLVED.** The doc-named file is
   `.../ModusToolboxProgtools-1.9/mtb-programmer/.../BT/CYBT_353027_EVAL/
   minidriver.hex`; its `20706A2_OCF.btp` / `CYW20706A2_IDFILE.txt` neighbors
   are exactly as `reflashing-the-module.md` §1a describes, and the SFLASH
   `.btp` names it `uart_legacy_ramcfg_only.hex` (the §7 name). It is
   **byte-identical to the BSP's `uart.hex`** and loads at `0x0D0200` (launch
   `0x0D0201`, Thumb). The docs' `0x00220000` is stale from an older SDK; §7
   also says to take the address from the hex's type-05 record, which
   `gen_blobs.py` does — so `0x0D0200` is correct, not a discrepancy.
2. **uart0 flow control — monitor on hardware.** `download.overlay` disables
   `hw-flow-control` for poll TX; if download-mode entry stalls, that's the
   first suspect.
3. **DS base — RESOLVED (see below): the app is now built for `0xFF003000`.**

## Flash-map: the SP-1 build targets DS `0xFF003000` (done)

The eval BSP `.btp` puts DS at `ConfigDSLocation = 0x4000` (→ `0xFF004000`),
but the **SP-1 module's Static Section points its DS base at `0xFF003000`**
(`findings.md`; VS is `0xFF001000..0xFF002000` per the btp, so `0xFF003000` is
safely above it). So the SP-1 artifact is built with a modified btp:

```
# 1. minidriver (Programming Tools; identical to the BSP uart.hex)
MD=".../ModusToolboxProgtools-1.9/mtb-programmer/ModusToolbox Programmer.app/\
Contents/mtb-programmer/BT/CYBT_353027_EVAL/minidriver.hex"

# 2. build the module app for DS 0xFF003000 (0x3000) instead of the eval 0x4000
cd ~/src/home-auto/sp-1-ble-radio/LE_Hello_Sensor
sed 's/ConfigDSLocation = 16384/ConfigDSLocation = 12288/' \
  .../TARGET_CYBT-353027-EVAL/release-*/CYBT-353027-EVAL-SFLASH.btp > CYBT-353027-EVAL-SP1.btp
make build CY_CORE_BTP="$PWD/CYBT-353027-EVAL-SP1.btp"   # -> DS available start 0xFF003000

# 3. embed minidriver + the 0xFF003000 DS image into the gitignored blob header
cd ~/src/home-auto/beacon-sp-1
scripts/gen_blobs.py --minidriver "$MD" \
  --ds ~/src/home-auto/sp-1-ble-radio/LE_Hello_Sensor/build/CYBT-353027-EVAL/Debug/BLE_HelloSensor_download.hex \
  --ds-base 0xFF003000

# 4. build the flasher (dry-run first, then armed)
scripts/fw.sh dl        # then, only after the dry-run passes on hardware:
scripts/fw.sh dlarm
```

`CYBT_DS_BASE` in `cybt_dl.h` is `0xFF003000` to match; the identity gate reads
the unit's live SS and refuses unless its type-0x02 DS pointer equals that. A
blob built for any other base is rejected by `ds_plan()` (verified). The btp's
`DLSectorEraseMode = "Chip erase"` only affects Infineon's ChipLoad, which this
flasher does not use — our path is WRITE_RAM DS-only, never chip-erase
(`README.md`'s safety headline). Build-verified: the armed flasher links the
`0xFF003000` DS image and passes the plan; not yet hardware-run.
