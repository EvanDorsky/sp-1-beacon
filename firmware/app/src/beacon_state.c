/* beacon_state.c — state-beacon payload codec (see beacon_state.h). Pure C. */
#include "beacon_state.h"

int beacon_state_encode(const struct beacon_state *s, uint8_t seq,
                        uint8_t *out, size_t cap)
{
    if (out == NULL || cap < BEACON_STATE_LEN) {
        return -1;
    }
    out[0] = BEACON_STATE_VER;
    out[1] = seq;
    out[2] = (uint8_t)(s->buttons & 0xFF);
    out[3] = (uint8_t)(s->buttons >> 8);
    for (int i = 0; i < 4; i++) {
        out[4 + i] = s->fader[i];
    }
    out[8] = s->battery;
    return BEACON_STATE_LEN;
}

static int diff_u8(uint8_t a, uint8_t b)
{
    return a > b ? a - b : b - a;
}

bool beacon_state_changed(const struct beacon_state *prev,
                          const struct beacon_state *cur)
{
    if (prev->buttons != cur->buttons) {
        return true;
    }
    for (int i = 0; i < 4; i++) {
        if (diff_u8(prev->fader[i], cur->fader[i]) > BEACON_FADER_DEADBAND) {
            return true;
        }
    }
    /* Battery creeps slowly; a 1-point flicker between ADC reads is noise. */
    if (diff_u8(prev->battery, cur->battery) > 1) {
        return true;
    }
    return false;
}
