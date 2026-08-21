#ifndef BTHOME_H
#define BTHOME_H
/*
 * bthome.h — BTHome v2 payload encoder + button-event classifier: THE wire
 * format (bluetooth/broadcast-format.md, spec at https://bthome.io/format/).
 *
 * The SP-1 broadcasts BTHome v2 service data (16-bit UUID 0xFCD2) that Home
 * Assistant's bthome integration decodes natively. Three parties touch it:
 *   - the nRF firmware (this codec) composes the service-data payload and
 *     pushes it to the module with WHCI_FELDD_SET_STATE,
 *   - the CYW20706 beacon app wraps it verbatim in a Service Data AD element
 *     (UUID prepended) inside a non-connectable advertisement,
 *   - Home Assistant discovers the device by MAC and turns the button events
 *     into event entities / device triggers.
 *
 * Payload layout (BTHome objects MUST be in ascending object-id order):
 *   [0]      device info    0x44 = BTHome v2, trigger-based, unencrypted
 *   [1..2]   0x00 <pid>     packet id: bumps per EVENT; receivers dedup on it
 *   [3..4]   0x01 <pct>     battery %, OMITTED entirely while unknown
 *   [5..22]  0x3A <ev> x9   one button-event object per button, FIXED order
 *                           (numbering is positional: always all 9, 0x00=none)
 *
 * Button order = the buttons.h logical indices (Play, T1..T4, Vol+, Vol-,
 * FWD, RWD) -> HA button_1..button_9.
 *
 * Events, not state bits: a tap emits `press` on release; a hold emits
 * `long_press` once at the threshold (its release then emits nothing). The
 * classifier here is pure (host-tested in firmware/test/test_bthome.c).
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BTHOME_SVC_UUID   0xFCD2u  /* Service Data 16-bit UUID (LE on the wire) */
#define BTHOME_DEVINFO    0x44     /* v2 (bits 5-7=010) | trigger-based (bit 2) */

#define BTHOME_OBJ_PID    0x00
#define BTHOME_OBJ_BATT   0x01
#define BTHOME_OBJ_BUTTON 0x3A

#define BTHOME_EV_NONE       0x00
#define BTHOME_EV_PRESS      0x01
#define BTHOME_EV_LONG_PRESS 0x04

#define BTHOME_BTN_COUNT  9
#define BTHOME_BATT_UNKNOWN 0xFF   /* battery arg: omit the battery object */

/* devinfo + pid + battery + 9 buttons (largest possible payload) */
#define BTHOME_MAX_PAYLOAD (1 + 2 + 2 + 2 * BTHOME_BTN_COUNT)

/* A press held at least this long is a long_press (emitted AT the threshold);
 * anything shorter is a press (emitted at release). */
#define BTHOME_LONG_MS 1000

/* Compose the service-data payload (devinfo + objects). ev is the 9 per-button
 * event values (BTHOME_EV_*); battery 0..100 or BTHOME_BATT_UNKNOWN to omit.
 * Returns the payload length, or -1 if it doesn't fit in cap. */
int bthome_encode(uint8_t pid, uint8_t battery,
                  const uint8_t ev[BTHOME_BTN_COUNT], uint8_t *out, size_t cap);

/* ---- press/long-press classifier (pure; drive from the debounced scan) ---- */
struct bthome_clf {
    int64_t down_t[BTHOME_BTN_COUNT];   /* uptime of the press edge, -1 = up */
    bool    long_sent[BTHOME_BTN_COUNT];
};

void bthome_clf_init(struct bthome_clf *c);

/* Feed one debounced edge. Returns the event to emit NOW (a release before the
 * long threshold -> BTHOME_EV_PRESS) or BTHOME_EV_NONE. */
uint8_t bthome_clf_edge(struct bthome_clf *c, int idx, bool pressed, int64_t now);

/* Per-tick: returns BTHOME_EV_LONG_PRESS exactly once when a held button
 * crosses BTHOME_LONG_MS, else BTHOME_EV_NONE. */
uint8_t bthome_clf_poll(struct bthome_clf *c, int idx, int64_t now);

/* True while button idx is (debounced-)down. */
static inline bool bthome_clf_down(const struct bthome_clf *c, int idx)
{
    return c->down_t[idx] >= 0;
}

#endif /* BTHOME_H */
