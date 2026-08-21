# SP-1 broadcast format — BTHome v2

> **STATUS: target spec (migration in progress).** The firmware on the device
> still broadcasts the legacy custom manufacturer-data format until the BTHome
> migration lands (see the plan at the bottom). The legacy format's spec lives
> in this file's git history (`git log -- bluetooth/broadcast-format.md`).

The SP-1 broadcasts **BTHome v2** (<https://bthome.io/format/>), the open BLE
advertising format Home Assistant discovers and decodes natively. No pairing,
no connection — HA's `bthome` integration auto-discovers the device as soon as
any HA Bluetooth radio/proxy hears it, and each button becomes an **event
entity / device trigger** you bind automations to. This replaces the custom
company-`0xFFFF` state payload (and with it, the stray-packet problem: BTHome
receivers key on device MAC + packet-id dedup, and can later be upgraded to
AES-CCM encryption for real spoof resistance).

## The advertisement

One legacy (31-byte) non-connectable advertisement:

```
02 01 06                          Flags (required — BlueZ won't parse without it)
1B 16 D2 FC <devinfo> <objects>   Service Data, 16-bit UUID 0xFCD2 (LE: D2 FC)
```

- **Device info byte = `0x44`**: version 2 (bits 5–7 = `010`), **trigger-based
  device** (bit 2 = 1 — we advertise on activity, not on an interval),
  unencrypted (bit 0 = 0).
- **Objects, in required numerical order (low → high id):**

| Object | Id | Size | Content |
|---|---|---|---|
| packet id | `0x00` | 2 B | `00 <pid>` — u8, bumps on every EVENT; receivers dedup on it |
| battery | `0x01` | 2 B | `01 <pct>` — u8 percent, sampled once per wake |
| button ×9 | `0x3A` | 18 B | `3A <event>` ×9, one per button in FIXED order (below) |

Total: 3 + 4 + 1 + 2 + 2 + 18 = **30 of 31 bytes**. Every packet carries all
nine button objects — **button numbering in BTHome is positional**, so the
count and order must never change between packets.

### Button order (BTHome button_1 … button_9)

| # | SP-1 button | # | SP-1 button | # | SP-1 button |
|---|---|---|---|---|---|
| 1 | Play | 4 | Track 3 | 7 | Vol − |
| 2 | Track 1 | 5 | Track 4 | 8 | FWD |
| 3 | Track 2 | 6 | Vol + | 9 | RWD |

(The **••** button is local power/function only — never broadcast.)

### Button event values (BTHome `0x3A`)

| Value | Event | SP-1 gesture |
|---|---|---|
| `0x00` | none | (placeholder — keeps positional numbering) |
| `0x01` | press | EVERY press, emitted instantly at the down edge |
| `0x04` | long_press | additionally, once the hold crosses ~1 s |
| `0x02` | double_press | *(not emitted initially — see plan)* |

> `press` fires at press-DOWN — latency beats long-press disambiguation — so a
> long hold emits `press` and then `long_press`. Bind them to non-conflicting
> actions on any button that uses both.

## Event semantics (vs. the old state bits)

The legacy format broadcast **held-state bits**; BTHome broadcasts **events**.
The nRF classifies each debounced press: `press` at the down edge, plus
`long_press` at the hold threshold. For each event it then:

1. bumps the **packet id**,
2. composes the packet with that button's event value (others `0x00`),
3. broadcasts it for the **on-air dwell** (~600 ms, ack-gated, exactly the
   existing latch machinery) so the receiver's duty-cycled scanner catches it,
4. keepalives during the linger window **reuse the same packet id** — BTHome
   receivers only process a *changed* pid, so repeats are free and harmless.

This is strictly more robust than the state bits: any single sighting of the
event packet delivers the press, dedup is the receiver's job (pid), and a
missed release can no longer un-press anything.

## Receiver (Home Assistant)

- Enable the Bluetooth integration (local radio or an ESPHome bluetooth proxy
  in range); the SP-1 appears automatically as a BTHome device (identified by
  its stable factory BD_ADDR — the module advertises without RPA/privacy).
- Each button surfaces as an event entity / device trigger
  (`button_1`…`button_9` per the table above) for automations; battery
  surfaces as a sensor.
- No local name is advertised (no room in the 31-byte packet; HA identifies by
  MAC). Rename the device in HA.

## Faders — a second packet type

There is no room for the four fader values alongside nine button objects, so
faders get their **own packet** on the same device (each packet drawing from
the one shared packet-id sequence):

```
02 01 06                          Flags
0F 16 D2 FC 44 00 <pid> 01 <batt> 09 <f1> 09 <f2> 09 <f3> 09 <f4>
```

Objects in id order: pid `0x00`, battery `0x01`, then **4× count-u8 (`0x09`)**
— a dimensionless 0–255 counter, exactly the fader range, positional like the
buttons (HA sensors `count`, `count_2`…`count_4`; rename to Fader 1–4). 20 of
31 bytes.

Behavior: a fader moved past the deadband (±3) wakes the device / marks it
dirty; while awake, fader packets stream at most every 200 ms with the latest
values, and **button events always take priority** (a fader update waits out
an event's dwell, never the other way round). Fader values freeze at last-good
while any button is held (rail-sag hardware constraint) and resume on release.
Receivers process each packet type independently — button entities update from
button packets, fader sensors from fader packets, dedup by pid as usual.

Binding brightness in HA is one automation: trigger on the fader sensor's
state change, action `light.turn_on` with
`brightness: {{ states('sensor.<fader>') | int }}` — 0–255 maps 1:1.

## Encryption (phase 3, optional)

BTHome supports AES-CCM with a 16-byte bindkey (device info bit 0 = 1); HA
prompts for the key on discovery. Turning it on makes packets unspoofable and
unreadable to neighbors — the definitive fix for stray-packet interference.

## Migration plan (tracking)

1. **nRF**: BTHome encoder (pure, host-tested) + press/long-press classifier +
   pid management; broadcast loop keeps the wake/ack/dwell machinery.
2. **CYW20706 app**: advertisement element switches from Manufacturer Data to
   **Service Data 0xFCD2** (`BTM_BLE_ADVERT_TYPE_SERVICE_DATA`); the
   WICED-HCI `SET_STATE` command becomes variable-length raw service-data
   payload from the nRF (app stays dumb). Requires one module rebuild +
   DS reflash with the existing M4 machinery (full-flash backup on hand).
3. **Tools**: `e2e_monitor.py` decodes BTHome; HomeSpan receiver retired for
   the SP-1 (HA takes over button automations).
