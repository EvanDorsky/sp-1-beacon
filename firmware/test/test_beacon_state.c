/* Host test for the state-beacon payload codec (beacon_state.c). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "beacon_state.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static void test_encode_layout(void)
{
    struct beacon_state s = {
        .buttons = 0x0113,          /* Play + Track1 + Track4 + RWD */
        .fader   = { 0, 127, 255, 4 },
        .battery = 87,
    };
    uint8_t buf[BEACON_STATE_LEN];
    CHECK(beacon_state_encode(&s, 42, buf, sizeof(buf)) == BEACON_STATE_LEN);
    const uint8_t want[BEACON_STATE_LEN] = {
        BEACON_STATE_VER, 42, 0x13, 0x01, 0, 127, 255, 4, 87,
    };
    CHECK(memcmp(buf, want, BEACON_STATE_LEN) == 0);
}

static void test_encode_too_small(void)
{
    struct beacon_state s = { 0 };
    uint8_t buf[BEACON_STATE_LEN - 1];
    CHECK(beacon_state_encode(&s, 0, buf, sizeof(buf)) == -1);
}

static void test_change_buttons(void)
{
    struct beacon_state a = { .buttons = 0, .battery = 50 };
    struct beacon_state b = a;
    CHECK(!beacon_state_changed(&a, &b));
    b.buttons = 1;                          /* any button edge is significant */
    CHECK(beacon_state_changed(&a, &b));
}

static void test_change_fader_deadband(void)
{
    struct beacon_state a = { .fader = { 100, 0, 0, 0 }, .battery = 50 };
    struct beacon_state b = a;
    b.fader[0] = 100 + BEACON_FADER_DEADBAND;      /* at the deadband: noise */
    CHECK(!beacon_state_changed(&a, &b));
    b.fader[0] = 100 + BEACON_FADER_DEADBAND + 1;  /* past it: a real move */
    CHECK(beacon_state_changed(&a, &b));
    b = a;
    b.fader[3] = 100;                              /* any fader counts */
    CHECK(beacon_state_changed(&a, &b));
}

static void test_change_battery(void)
{
    struct beacon_state a = { .battery = 50 };
    struct beacon_state b = a;
    b.battery = 51;                         /* 1-point flicker: noise */
    CHECK(!beacon_state_changed(&a, &b));
    b.battery = 53;
    CHECK(beacon_state_changed(&a, &b));
    b = a;
    b.battery = BEACON_BATTERY_UNKNOWN;     /* unknown -> known is a change */
    CHECK(beacon_state_changed(&a, &b));
}

int main(void)
{
    test_encode_layout();
    test_encode_too_small();
    test_change_buttons();
    test_change_fader_deadband();
    test_change_battery();
    if (g_fail) {
        printf("test_beacon_state: FAIL\n");
        return 1;
    }
    printf("test_beacon_state: all passed\n");
    return 0;
}
