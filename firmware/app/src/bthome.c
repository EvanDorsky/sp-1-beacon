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
        c->down_t[idx] = now;
        c->long_sent[idx] = false;
        return BTHOME_EV_NONE;
    }
    /* Release: a short hold is a press; a long hold already emitted its
     * long_press at the threshold, so its release emits nothing. */
    bool was_down = c->down_t[idx] >= 0;
    bool already_long = c->long_sent[idx];
    c->down_t[idx] = -1;
    c->long_sent[idx] = false;
    return (was_down && !already_long) ? BTHOME_EV_PRESS : BTHOME_EV_NONE;
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
