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

## Status: M3a (broadcast state machine, nRF side)

The firmware is now the product's shape: the SP-1 idles in low power (radio in
reset, LEDs dark, the control rail duty-cycled around a ~40 ms scan) and wakes
on **any activity — a button held or a fader moved**. While awake it
broadcasts the full control state and refreshes it on every change; ~5 s after
the last activity it goes back to sleep. Faders freeze at last-good while a
button is held (a pressed button sags the shared rail and corrupts fader
reads — feldd's bench finding); fader moves register whenever no button is
down, including from idle, where a move alone wakes the radio.

The wire payload (`beacon_state.h`, 9 bytes): version, seq, a 9-bit button
bitmap, four 8-bit faders, battery percent. It is pushed to the module with a
private `SET_STATE` WICED-HCI command that the **M3b beacon module app** will
embed in a non-connectable advertisement. Until that app is flashed, the
module still runs feldd's BLE-MIDI app, which ignores `SET_STATE` — so today a
wake broadcasts feldd's presence advertisement instead (same state machine,
same timing, same power behavior; `./scripts/fw.sh e2e` sees it on the air).

Extras that survive from the bring-up builds: `•• hold ~5 s` = power off (••
wakes), a short `••` tap logs full state to the console, `Track 1+4` held ~3 s
= DFU escape into the TE bootloader, and the USB console only comes up when a
cable is present (idle power).

Next: **M3b** the CYW20706 beacon app (ModusToolbox), **M4** the SS-preserving
module reflash rebuilt from the docs in `bluetooth/`, **M5** the ESP32/HomeSpan
receiver, **M6** power measurement + tuning.

## Building

Zephyr / nRF Connect SDK app for the nRF52840, board definition `sp1` from
[marisko](https://github.com/softmodded/marisko):

```sh
./scripts/setup-zephyr-ws.sh   # one-time: west workspace + SDK + marisko clone (multi-GB)
./scripts/fw.sh build          # build
./scripts/fw.sh bin            # flashable sp1_beacon.bin
./scripts/fw.sh test           # host unit tests (buttons decode, WICED-HCI codec)
```

Flashing (either way — put the SP-1 in bootloader mode first: power off, hold
Track 1+4, plug USB):

- **CLI:** `./scripts/fw.sh flash` — uses [rome](https://github.com/softmodded/rome),
  a Rust CLI that drives the TE bootloader's serial (the same protocol
  solderless uses). Build it once: `brew install libusb && (cd ~/src/rome &&
  cargo build --release)`; override the binary path with `ROME=...` or the port
  with `./scripts/fw.sh flash /dev/cu.usbmodemXXX`.
- **Browser:** upload the `.bin` at [Solderless](https://solderless.engineering).

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
