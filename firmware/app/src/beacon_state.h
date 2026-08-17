#ifndef BEACON_STATE_H
#define BEACON_STATE_H
/*
 * beacon_state.h — the SP-1 state-beacon payload: THE wire format.
 *
 * This is the single source of truth for the bytes that go over the air as
 * BLE manufacturer-specific data. Three parties encode/decode it:
 *   - the nRF firmware (this repo) builds it and pushes it to the module
 *     with the WHCI_FELDD_SET_STATE command,
 *   - the CYW20706 beacon app (M3b, ModusToolbox) embeds it verbatim in a
 *     non-connectable advertisement,
 *   - the receiver (ESP32/HomeSpan) parses it out of the advertisement.
 *
 * Layout (BEACON_STATE_LEN = 9 bytes, all multi-byte fields little-endian):
 *   [0] version        (BEACON_STATE_VER)
 *   [1] seq            bumps on every SIGNIFICANT state change; receivers
 *                      dedupe repeated advertisements by it
 *   [2..3] buttons u16 bit i = button held, indices per buttons.h:
 *                      0=Play 1..4=Track1..4 5=VolUp 6=VolDn 7=FWD 8=RWD
 *                      (•• is the power button and is not reported)
 *   [4..7] fader 1..4  u8, 0..255 (12-bit SAADC >> 4, rail-sag compensated)
 *   [8] battery        percent 0..100, or 0xFF = not yet measured
 *
 * Pure C, host-tested in firmware/test/test_beacon_state.c.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BEACON_STATE_VER 1
#define BEACON_STATE_LEN 9
#define BEACON_BATTERY_UNKNOWN 0xFF

/* Fader movement below this (on the 0..255 scale) is ADC noise, not a user
 * gesture: it does not count as a significant change and never bumps seq. */
#define BEACON_FADER_DEADBAND 3

struct beacon_state {
    uint16_t buttons;
    uint8_t  fader[4];
    uint8_t  battery;
};

/* Encode into out (cap >= BEACON_STATE_LEN). Returns BEACON_STATE_LEN, or -1
 * if it doesn't fit. */
int beacon_state_encode(const struct beacon_state *s, uint8_t seq,
                        uint8_t *out, size_t cap);

/* True if cur differs from prev enough to bump seq and re-send: any button
 * edge, a fader moved past the deadband, or the battery reading changed by
 * more than 1 point. */
bool beacon_state_changed(const struct beacon_state *prev,
                          const struct beacon_state *cur);

#endif /* BEACON_STATE_H */
