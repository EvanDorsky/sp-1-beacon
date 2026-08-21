/* test_bthome.c — host tests for the BTHome v2 codec + classifier (bthome.c).
 * Golden byte layouts checked against https://bthome.io/format/. */
#include <stdio.h>
#include <string.h>
#include "bthome.h"

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

static void test_encode_full(void)
{
    uint8_t ev[BTHOME_BTN_COUNT] = {0};
    ev[1] = BTHOME_EV_PRESS;              /* button_2 (Track 1) pressed */
    uint8_t out[BTHOME_MAX_PAYLOAD];
    int n = bthome_encode(5, 97, ev, out, sizeof(out));

    CHECK(n == BTHOME_MAX_PAYLOAD, "full payload length 23");
    CHECK(out[0] == 0x44, "devinfo: v2 + trigger-based");
    CHECK(out[1] == 0x00 && out[2] == 5, "packet id object");
    CHECK(out[3] == 0x01 && out[4] == 97, "battery object");
    /* nine 0x3A objects, only index 1 = press */
    for (int i = 0; i < BTHOME_BTN_COUNT; i++) {
        CHECK(out[5 + 2 * i] == 0x3A, "button object id");
        CHECK(out[6 + 2 * i] == (i == 1 ? 0x01 : 0x00), "button event value");
    }
    /* whole-advertisement budget: flags(3) + svc hdr(2) + uuid(2) + payload */
    CHECK(3 + 2 + 2 + n <= 31, "fits a legacy 31-byte advertisement");
}

static void test_encode_no_battery(void)
{
    uint8_t ev[BTHOME_BTN_COUNT] = {0};
    uint8_t out[BTHOME_MAX_PAYLOAD];
    int n = bthome_encode(0, BTHOME_BATT_UNKNOWN, ev, out, sizeof(out));

    CHECK(n == BTHOME_MAX_PAYLOAD - 2, "battery omitted while unknown");
    CHECK(out[1] == 0x00, "pid object still first");
    CHECK(out[3] == 0x3A, "buttons follow pid directly");
}

static void test_encode_clamps_and_caps(void)
{
    uint8_t ev[BTHOME_BTN_COUNT] = {0};
    uint8_t out[BTHOME_MAX_PAYLOAD];
    CHECK(bthome_encode(0, 250, ev, out, sizeof(out)) > 0 && out[4] == 100,
          "battery clamped to 100");
    CHECK(bthome_encode(0, 50, ev, out, 10) == -1, "small cap refused");
    CHECK(bthome_encode(0, 50, NULL, out, sizeof(out)) == -1, "NULL ev refused");
}

static void test_encode_faders(void)
{
    uint8_t f[BTHOME_FADER_COUNT] = { 0, 128, 255, 7 };
    uint8_t out[BTHOME_MAX_PAYLOAD];
    int n = bthome_encode_faders(9, 55, f, out, sizeof(out));

    CHECK(n == BTHOME_FADER_PAYLOAD, "fader payload length 13");
    CHECK(out[0] == 0x44, "devinfo");
    CHECK(out[1] == 0x00 && out[2] == 9, "pid object");
    CHECK(out[3] == 0x01 && out[4] == 55, "battery object");
    for (int i = 0; i < BTHOME_FADER_COUNT; i++) {
        CHECK(out[5 + 2 * i] == 0x09, "count object id");
        CHECK(out[6 + 2 * i] == f[i], "count value");
    }
    CHECK(3 + 2 + 2 + n <= 31, "fader packet fits a legacy advertisement");

    n = bthome_encode_faders(9, BTHOME_BATT_UNKNOWN, f, out, sizeof(out));
    CHECK(n == BTHOME_FADER_PAYLOAD - 2 && out[3] == 0x09,
          "battery omitted while unknown");
    CHECK(bthome_encode_faders(9, 55, f, out, 5) == -1, "small cap refused");
}

static void test_classifier_tap(void)
{
    struct bthome_clf c;
    bthome_clf_init(&c);

    CHECK(bthome_clf_edge(&c, 3, true, 1000) == BTHOME_EV_PRESS,
          "press fires INSTANTLY at the down edge");
    CHECK(bthome_clf_down(&c, 3), "down while held");
    CHECK(bthome_clf_poll(&c, 3, 1500) == BTHOME_EV_NONE, "no long before threshold");
    CHECK(bthome_clf_edge(&c, 3, false, 1600) == BTHOME_EV_NONE, "release emits nothing");
    CHECK(!bthome_clf_down(&c, 3), "up after release");
}

static void test_classifier_long(void)
{
    struct bthome_clf c;
    bthome_clf_init(&c);

    CHECK(bthome_clf_edge(&c, 0, true, 0) == BTHOME_EV_PRESS, "press at down, even for a hold");
    CHECK(bthome_clf_poll(&c, 0, BTHOME_LONG_MS - 1) == BTHOME_EV_NONE, "just under threshold");
    CHECK(bthome_clf_poll(&c, 0, BTHOME_LONG_MS) == BTHOME_EV_LONG_PRESS, "long at threshold");
    CHECK(bthome_clf_poll(&c, 0, BTHOME_LONG_MS + 500) == BTHOME_EV_NONE, "long fires once");
    CHECK(bthome_clf_edge(&c, 0, false, BTHOME_LONG_MS + 900) == BTHOME_EV_NONE,
          "release after long emits nothing");
    CHECK(bthome_clf_edge(&c, 0, true, 5000) == BTHOME_EV_PRESS, "re-press fires again");
}

static void test_classifier_independent_buttons(void)
{
    struct bthome_clf c;
    bthome_clf_init(&c);

    CHECK(bthome_clf_edge(&c, 2, true, 0) == BTHOME_EV_PRESS, "button 2 press at down");
    CHECK(bthome_clf_edge(&c, 7, true, 100) == BTHOME_EV_PRESS, "button 7 press at down");
    CHECK(bthome_clf_edge(&c, 2, false, 200) == BTHOME_EV_NONE, "button 2 release quiet");
    CHECK(bthome_clf_poll(&c, 7, 100 + BTHOME_LONG_MS) == BTHOME_EV_LONG_PRESS,
          "button 7 long, independent state");
    CHECK(bthome_clf_edge(&c, -1, false, 0) == BTHOME_EV_NONE, "bad idx safe");
    CHECK(bthome_clf_edge(&c, BTHOME_BTN_COUNT, true, 0) == BTHOME_EV_NONE, "oob idx safe");
}

int main(void)
{
    test_encode_full();
    test_encode_no_battery();
    test_encode_clamps_and_caps();
    test_encode_faders();
    test_classifier_tap();
    test_classifier_long();
    test_classifier_independent_buttons();

    if (failures) {
        printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("all bthome tests passed\n");
    return 0;
}
