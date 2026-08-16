# beacon-sp-1

**Firmware that turns the Teenage Engineering SP-1 into a battery-powered BLE
beacon remote for home automation: press a button, a beacon goes on the air.**

> The SP-1 is an unreleased TE device. This is unofficial community firmware,
> not affiliated with or endorsed by Teenage Engineering. It is a stripped-down
> fork of [feldd](https://feldd.com) ([bnjreece/feldd-sp1-firmware](https://github.com/bnjreece/feldd-sp1-firmware)),
> whose MIDI/keyboard controller features were removed; what remains is the
> button scanning, the boot-safety plumbing, and the Bluetooth-module link.

## How it works (two chips)

The SP-1's nRF52840 (where this firmware runs) has **no antenna** — all
wireless goes through the onboard Infineon **CYW20706** Bluetooth module, which
feldd reflashes with a custom BLE app. This firmware drives that module over
the WICED-HCI UART: buttons are scanned on the nRF, and each press tells the
module what to put on the air. The full architecture, the module reflash
method, and the hardware evidence live in [`bluetooth/`](bluetooth) (inherited
from feldd — read `bluetooth/README.md` first, and mind its safety headline:
**never chip-erase the module**).

## Status: M1 + M2 (bench bring-up)

Current firmware assumes the module already carries **feldd's BLE-MIDI/HID
module app** (flashed by feldd 0.27+). It exercises that app's private
WICED-HCI command group:

| Control | Action |
|---------|--------|
| Track 1–4 | ~2 s advertising burst (presence beacon; boots the module first if needed) |
| Play | PING the module app |
| Vol +/− | Advertising on / off (manual) |
| FWD / RWD | Module boot / module into reset (BT hard-off) |
| •• hold ~5 s | Power off (SYSTEM_OFF; •• wakes) |
| Track 1+4 hold ~1.2 s | DFU — reboot into the TE bootloader for reflashing |

Every button edge and every WICED-HCI frame (both directions) is logged on the
USB-CDC console (`./scripts/fw.sh monitor`).

Planned next (see the milestone plan in the project discussion): a dedicated
beacon module app (non-connectable advertising with a per-button payload), the
SS-preserving module reflash path rebuilt from the docs in `bluetooth/`, and
the low-power idle (module held in reset, relaxed scan cadence).

## Building

Zephyr / nRF Connect SDK app for the nRF52840, board definition `sp1` from
[marisko](https://github.com/softmodded/marisko):

```sh
./scripts/setup-zephyr-ws.sh   # one-time: west workspace + SDK + marisko clone (multi-GB)
./scripts/fw.sh build          # build
./scripts/fw.sh bin            # flashable sp1_beacon.bin
./scripts/fw.sh test           # host unit tests (buttons decode, WICED-HCI codec)
```

Flash with the community [Solderless](https://solderless.engineering) tool:
power off, hold Track 1+4, plug USB, upload the `.bin`.

## ⚠️ Flashing safety

You're modifying an irreplaceable device. This firmware keeps feldd's safety
conventions: it links above the TE bootloader (`0x20000`), keeps the Track 1+4
DFU escape hatch and the charge-standby gate, and enables the battery charger's
/CE line. A bad flash is recoverable by re-flashing a known-good image via the
bootloader. The Bluetooth module side has its own, stricter rule — see
`bluetooth/README.md`.

## Credits

- **[feldd](https://feldd.com)** — the parent project: everything here
  descends from it, including the entire `bluetooth/` documentation set
- **[Solderless](https://solderless.engineering)** — the flash + stem-loader tools
- **[Tim Knapen / SP-1-dev](https://github.com/timknapen/SP-1-dev)** — SP-1 developer docs + wiki
- **[marisko](https://github.com/softmodded/marisko)** — the `sp1` Zephyr board definition
- **Eric Lewis / sp1-midi** — gold-standard nRF52 BSP + boot-safety reference

## License

MIT, see [`LICENSE`](LICENSE). Dependencies (Zephyr, the board definition,
etc.) under their own licenses; see [`NOTICE`](NOTICE).
