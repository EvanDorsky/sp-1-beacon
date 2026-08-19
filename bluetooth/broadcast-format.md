# SP-1 beacon broadcast format

The over-the-air contract between the SP-1 and its receiver. This is the spec the
**ESP32 / HomeSpan** side implements against. The firmware source of truth is
[`firmware/app/src/beacon_state.h`](../firmware/app/src/beacon_state.h) — if the
two ever disagree, the header wins and this doc is stale.

## The chain

```
button/fader  ->  nRF52840  ->  WICED-HCI (SET_STATE)  ->  CYW20706 beacon app
                                                                   |
                                                    non-connectable BLE advertisement
                                                                   |
                                                                   v
                                                         ESP32 / HomeSpan (this doc)
```

The nRF never has an antenna; all radio is the CYW20706 module. The module embeds
the 9-byte state payload verbatim as **manufacturer-specific data** and broadcasts
it. No pairing, no connection, no GATT — the receiver is a passive scanner.

## Advertisement structure

A standard BLE ADV_NONCONN_IND payload with two AD structures:

| AD type | Name | Bytes | Value |
|---|---|---|---|
| `0x01` | Flags | `02 01 04` | `LE General`, BR/EDR not supported (`0x04`) |
| `0xFF` | Manufacturer Specific Data | `0C FF FF FF <9-byte payload>` | company `0xFFFF` (LE) + payload |

The manufacturer-data AD element, byte for byte:

```
0C            length = 12 (1 type + 2 company + 9 payload)
FF            AD type = Manufacturer Specific Data
FF FF         company identifier 0xFFFF, little-endian
<9 bytes>     the beacon_state payload (below)
```

> **Company `0xFFFF`** is the Bluetooth SIG "reserved / internal use" identifier —
> intentionally not a real vendor. It's fine for a private device, but any other
> "testing" beacon nearby can also use `0xFFFF`, so the receiver **must** also
> check the version byte (payload[0] == 1) before trusting the packet. See
> [Hardening](#hardening-optional) for making the match unique.

## The 9-byte payload

All multi-byte fields are **little-endian**. `BEACON_STATE_LEN = 9`.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| `[0]` | version | `u8` | `0x01` (`BEACON_STATE_VER`). Bump only on a breaking layout change. |
| `[1]` | seq | `u8` | Increments on each **significant** change; wraps mod 256. Dedup key. |
| `[2..3]` | buttons | `u16` LE | Bitmap, `bit i = button i held` (see below). |
| `[4]` | fader 1 | `u8` | `0..255` |
| `[5]` | fader 2 | `u8` | `0..255` |
| `[6]` | fader 3 | `u8` | `0..255` |
| `[7]` | fader 4 | `u8` | `0..255` |
| `[8]` | battery | `u8` | percent `0..100`, or `0xFF` = not yet measured (`BEACON_BATTERY_UNKNOWN`) |

### Button bitmap (`[2..3]`, 9 bits used)

| Bit | Button | Bit | Button |
|---|---|---|---|
| 0 | Play | 5 | Vol+ |
| 1 | Track 1 | 6 | Vol− |
| 2 | Track 2 | 7 | FWD |
| 3 | Track 3 | 8 | RWD |
| 4 | Track 4 | | |

`bit = 1` while the button is **held**. The **••** button is the power/function
button and is **not** reported (it powers the device on/off locally).

### Faders (`[4..7]`)

Each is a 12-bit SAADC reading shifted right by 4 → `0..255`. Values are
rail-sag compensated and frozen at last-good while any button is held (a held
button loads the shared analog rail and corrupts fader reads), so treat a fader
value as valid "current position" but don't expect it to update *during* a
button hold. Movement smaller than `BEACON_FADER_DEADBAND` (3, on the 0..255
scale) is noise and does not bump `seq`.

### Battery (`[8]`)

`0..100` percent, or `0xFF` if the firmware hasn't sampled it yet this wake.
Sampled roughly every 5 s while broadcasting.

## Timing & behavior — how the burst works

The SP-1 is **not** a continuous beacon. To save power it keeps the radio in
reset when idle, so **there is nothing on the air until something happens**:

1. A button press or a fader move past the deadband **wakes** the module.
2. The module advertises **non-connectable, high duty** (fast interval, tens of
   ms) — this is a *burst*.
3. During the burst the payload is refreshed **on every change**, plus a
   **keepalive** re-send of the unchanged state about **once per second**.
4. ~**5 s after the last activity** the firmware stops advertising and powers the
   module back down. The burst ends.

Consequences for the receiver:

- **You see bursts, not a heartbeat.** Absence of advertisements means "idle,"
  not "gone." Don't treat a scan gap as a fault.
- **Every advert carries the full state.** The payload is absolute, not a delta —
  the latest packet you've received is always the complete current state. You
  never need to reconstruct state from history.
- **`seq` distinguishes news from keepalive.** Same `seq` re-sent = nothing
  changed (a keepalive, or just multiple sightings of one advert). A new `seq` =
  a real change (a new burst starts with a bumped `seq`, and each in-burst change
  bumps it again). Dedup on `seq` to avoid processing the same state repeatedly.
- **A button release shows up as buttons going to 0** in the final adverts of the
  burst — you get the release, then the burst ends.

## Receiver guidance (ESP32 / HomeSpan)

### Match + parse

```c
// Given the raw manufacturer-data bytes of one advertisement (the value of the
// 0xFF AD element, INCLUDING the 2 company-id bytes), decode the SP-1 state.
// Returns false if this isn't an SP-1 beacon packet.
#define SP1_COMPANY_ID 0xFFFF
#define SP1_STATE_VER  0x01

typedef struct {
    uint8_t  seq;
    uint16_t buttons;   // bit i = button i held
    uint8_t  fader[4];  // 0..255
    uint8_t  battery;   // 0..100, or 0xFF = unknown
} sp1_state_t;

bool sp1_parse(const uint8_t *mfr, size_t len, sp1_state_t *out) {
    if (len < 2 + 9) return false;                     // company + 9-byte payload
    uint16_t company = mfr[0] | (mfr[1] << 8);
    if (company != SP1_COMPANY_ID) return false;
    const uint8_t *p = mfr + 2;                        // skip company id
    if (p[0] != SP1_STATE_VER) return false;           // version gate (important!)
    out->seq     = p[1];
    out->buttons = p[2] | (p[3] << 8);
    out->fader[0] = p[4]; out->fader[1] = p[5];
    out->fader[2] = p[6]; out->fader[3] = p[7];
    out->battery = p[8];
    return true;
}
```

> Some BLE stacks hand you manufacturer data **without** the AD length/type prefix
> but **with** the company id (as above); a few strip the company id too. Confirm
> what your scan API gives you and adjust the offset. The `e2e_monitor.py` decoder
> in this repo expects `{company_id: payload_bytes}` (company already split off).

### Edge detection (for momentary HomeKit switches)

Keep the last accepted `buttons` bitmap. On a newly-received state (new `seq`),
a `0 -> 1` transition of a bit is a **press event**; `1 -> 0` is a **release**.
Because the payload is absolute, XOR the old and new bitmaps to find what changed:

```c
uint16_t changed = prev_buttons ^ new_buttons;
uint16_t pressed = changed & new_buttons;    // bits that went 0->1
uint16_t released = changed & prev_buttons;  // bits that went 1->0
```

### Suggested HomeKit mapping (HomeSpan)

- **Buttons** → `StatelessProgrammableSwitch` (one per button), firing a
  single-press event on the `0->1` edge. Great for "Play toggles the living-room
  scene," etc.
- **Faders** → a `LightBulb` Brightness (or a custom characteristic) scaled
  `0..255 -> 0..100`, updated whenever the value changes past the deadband.
- **Battery** → `BatteryService` (`BatteryLevel`, `StatusLowBattery`), skipping
  updates when the value is `0xFF`.

### Don'ts

- Don't connect / pair — it's non-connectable; a connection attempt just wastes
  power and can shorten the burst.
- Don't require a steady stream — process each accepted state and idle otherwise.
- Don't trust `0xFFFF` alone — always gate on the version byte.

## Hardening (optional)

For a more collision-proof match if `0xFFFF` beacons ever clash in your RF
environment, future firmware could either move to an assigned 16-bit company id
or switch from manufacturer data to **Service Data** under a random 128-bit UUID
(unique by construction). Either is a payload change that would bump
`BEACON_STATE_VER`; this doc and the receiver would update together. Not needed
today.

## Worked example

Play + Track 3 held, faders at 128/0/255/64, battery 87%, seq 42:

```
buttons = bit0 (Play) | bit3 (Track3) = 0x0009
payload = 01 2A 09 00 80 00 FF 40 57
          ^  ^  ^^^^^ ^  ^  ^  ^  ^
          |  |  |     f1 f2 f3 f4 batt=0x57=87
          |  |  buttons 0x0009 LE
          |  seq = 0x2A = 42
          version 1

full mfr-data AD element: 0C FF FF FF 01 2A 09 00 80 00 FF 40 57
```

## References

- [`firmware/app/src/beacon_state.h`](../firmware/app/src/beacon_state.h) — the
  canonical layout + `beacon_state_encode()`.
- [`firmware/app/src/main.c`](../firmware/app/src/main.c) — the broadcast state
  machine (wake / linger / keepalive / seq).
- [`scripts/e2e_monitor.py`](../scripts/e2e_monitor.py) — a working host decoder
  you can copy the parse logic from.
- [`bluetooth/module-app/sp1-beacon-app.md`](module-app/sp1-beacon-app.md) — the
  CYW20706 side that builds the advertisement.
