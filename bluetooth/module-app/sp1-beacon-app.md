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

## Flash-map note for the reflash (M4)

The stock build targets the eval board's `.btp`: `ConfigDSLocation = 0x4000`.
**The SP-1 module's Static Section points its DS base at `0xFF003000`**
(`findings.md`, and feldd's flasher used DS_FLOOR 0xFF003000), so the SP-1
artifact must be built with `ConfigDSLocation = 0x3000` (or post-processed),
and the flasher's identity gate must verify the unit's actual SS type-0x02 DS
pointer matches the image's base before writing. The BSP also ships a
`CYBT-353027-EVAL-SFLASH.btp` variant (the default btp says
`DLConfigTargeting = "EEPROM"`); which targeting feldd's build used is not
recorded — resolve against the SS dump before the armed write. As always:
DS-only Upgrade Download, never chip-erase (`README.md`'s safety headline).
