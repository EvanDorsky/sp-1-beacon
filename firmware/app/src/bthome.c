/* bthome.c — BTHome v2 payload codec + button-event classifier (see bthome.h).
 * Pure C, host-tested in firmware/test/test_bthome.c. */
#include "bthome.h"

int bthome_encode(uint8_t pid, uint8_t battery,
                  const uint8_t ev[BTHOME_BTN_COUNT], uint8_t *out, size_t cap)
{
    size_t need = 1 + 2 + 2 * BTHOME_BTN_COUNT +
                  (battery != BTHOME_BATT_UNKNOWN ? 2 : 0);

    if (out == NULL || ev == NULL || cap < need) {
        return -1;
    }

    size_t n = 0;
    out[n++] = BTHOME_DEVINFO;
    out[n++] = BTHOME_OBJ_PID;
    out[n++] = pid;
    if (battery != BTHOME_BATT_UNKNOWN) {
        out[n++] = BTHOME_OBJ_BATT;
        out[n++] = battery > 100 ? 100 : battery;
    }
    for (int i = 0; i < BTHOME_BTN_COUNT; i++) {
        out[n++] = BTHOME_OBJ_BUTTON;
        out[n++] = ev[i];
    }
    return (int)n;
}

int bthome_encode_faders(uint8_t pid, uint8_t battery,
                         const uint8_t fader[BTHOME_FADER_COUNT],
                         uint8_t *out, size_t cap)
{
    size_t need = 1 + 2 + 2 * BTHOME_FADER_COUNT +
                  (battery != BTHOME_BATT_UNKNOWN ? 2 : 0);

    if (out == NULL || fader == NULL || cap < need) {
        return -1;
    }

    size_t n = 0;
    out[n++] = BTHOME_DEVINFO;
    out[n++] = BTHOME_OBJ_PID;
    out[n++] = pid;
    if (battery != BTHOME_BATT_UNKNOWN) {
        out[n++] = BTHOME_OBJ_BATT;
        out[n++] = battery > 100 ? 100 : battery;
    }
    for (int i = 0; i < BTHOME_FADER_COUNT; i++) {
        out[n++] = BTHOME_OBJ_COUNT;
        out[n++] = fader[i];
    }
    return (int)n;
}

void bthome_clf_init(struct bthome_clf *c)
{
    for (int i = 0; i < BTHOME_BTN_COUNT; i++) {
        c->down_t[i] = -1;
        c->long_sent[i] = false;
    }
}

uint8_t bthome_clf_edge(struct bthome_clf *c, int idx, bool pressed, int64_t now)
{
    if (idx < 0 || idx >= BTHOME_BTN_COUNT) {
        return BTHOME_EV_NONE;
    }
    if (pressed) {
        /* press fires IMMEDIATELY at press-down (latency matters more than
         * suppressing it under a long hold: a hold emits press THEN long_press,
         * so don't bind conflicting actions to both on one button). */
        c->down_t[idx] = now;
        c->long_sent[idx] = false;
        return BTHOME_EV_PRESS;
    }
    /* Release emits nothing: press already went out at the down edge and
     * long_press (if any) at its threshold. */
    c->down_t[idx] = -1;
    c->long_sent[idx] = false;
    return BTHOME_EV_NONE;
}

uint8_t bthome_clf_poll(struct bthome_clf *c, int idx, int64_t now)
{
    if (idx < 0 || idx >= BTHOME_BTN_COUNT) {
        return BTHOME_EV_NONE;
    }
    if (c->down_t[idx] >= 0 && !c->long_sent[idx] &&
        now - c->down_t[idx] >= BTHOME_LONG_MS) {
        c->long_sent[idx] = true;
        return BTHOME_EV_LONG_PRESS;
    }
    return BTHOME_EV_NONE;
}
