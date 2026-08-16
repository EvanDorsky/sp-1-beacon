/*
 * Host test for the pure WICED-HCI wire codec (wiced_hci.c).
 *
 * The wire format is grounded in the one frame captured on real hardware
 * (bluetooth/hardware-and-architecture.md §5): the LE-group passive-scan
 * command goes 19 01 01 01 00 01 — type 0x19, opcode u16 LE (group<<8|code),
 * length u16 LE, payload. H4 events are the download ROM's ack shape:
 * 04 0E 04 01 03 0C 00.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wiced_hci.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

/* Feed a byte string through the streaming parser; returns number of frames,
 * copies the last one (payload included) into *last. */
static uint8_t last_payload[WHCI_MAX_PAYLOAD];
static int feed(struct whci_parser *p, const uint8_t *bytes, size_t n,
                struct whci_frame *last)
{
    int frames = 0;
    struct whci_frame f;
    for (size_t i = 0; i < n; i++) {
        if (whci_parse_byte(p, bytes[i], &f)) {
            frames++;
            if (last) {
                *last = f;
                memcpy(last_payload, f.payload, f.len);
                last->payload = last_payload;
            }
        }
    }
    return frames;
}

static void test_build_ping(void)
{
    /* FELDD PING (group 0xF0, code 0x03), no payload. */
    uint8_t buf[8];
    int n = whci_build(buf, sizeof(buf), WHCI_OPCODE(WHCI_GROUP_FELDD, WHCI_FELDD_PING), 0, 0);
    const uint8_t want[] = { 0x19, 0x03, 0xF0, 0x00, 0x00 };
    CHECK(n == 5);
    CHECK(memcmp(buf, want, 5) == 0);
}

static void test_build_adv_on(void)
{
    /* FELDD ADV (code 0x04) with a 1-byte enable payload. */
    uint8_t buf[8];
    uint8_t on = 1;
    int n = whci_build(buf, sizeof(buf), WHCI_OPCODE(WHCI_GROUP_FELDD, WHCI_FELDD_ADV), &on, 1);
    const uint8_t want[] = { 0x19, 0x04, 0xF0, 0x01, 0x00, 0x01 };
    CHECK(n == 6);
    CHECK(memcmp(buf, want, 6) == 0);
}

static void test_build_matches_captured_le_scan(void)
{
    /* The hardware-captured LE-group scan command: 19 01 01 01 00 01. */
    uint8_t buf[8];
    uint8_t en = 1;
    int n = whci_build(buf, sizeof(buf), WHCI_OPCODE(0x01, 0x01), &en, 1);
    const uint8_t want[] = { 0x19, 0x01, 0x01, 0x01, 0x00, 0x01 };
    CHECK(n == 6);
    CHECK(memcmp(buf, want, 6) == 0);
}

static void test_build_too_small(void)
{
    uint8_t buf[4];
    CHECK(whci_build(buf, sizeof(buf), 0xF003, 0, 0) == -1);
}

static void test_parse_wiced_roundtrip(void)
{
    /* Build->parse roundtrip, streamed one byte at a time. */
    struct whci_parser p;
    struct whci_frame f;
    uint8_t payload[] = { 0xAA, 0xBB, 0xCC };
    uint8_t buf[16];
    int n = whci_build(buf, sizeof(buf), WHCI_OPCODE(0xF0, 0x42), payload, 3);

    whci_parser_init(&p);
    CHECK(feed(&p, buf, (size_t)n, &f) == 1);
    CHECK(f.kind == WHCI_PKT_WICED);
    CHECK(WHCI_GROUP(f.opcode) == 0xF0);
    CHECK(WHCI_CODE(f.opcode) == 0x42);
    CHECK(f.len == 3);
    CHECK(memcmp(f.payload, payload, 3) == 0);
    CHECK(p.garbage_bytes == 0 && p.dropped_frames == 0);
}

static void test_parse_zero_len_frame(void)
{
    struct whci_parser p;
    struct whci_frame f;
    const uint8_t bytes[] = { 0x19, 0x03, 0xF0, 0x00, 0x00 };
    whci_parser_init(&p);
    CHECK(feed(&p, bytes, sizeof(bytes), &f) == 1);
    CHECK(f.kind == WHCI_PKT_WICED && f.opcode == 0xF003 && f.len == 0);
}

static void test_parse_h4_event(void)
{
    /* The download ROM's HCI_RESET command-complete ack. */
    struct whci_parser p;
    struct whci_frame f;
    const uint8_t bytes[] = { 0x04, 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00 };
    const uint8_t want[] = { 0x01, 0x03, 0x0C, 0x00 };
    whci_parser_init(&p);
    CHECK(feed(&p, bytes, sizeof(bytes), &f) == 1);
    CHECK(f.kind == WHCI_PKT_HCI_EVT);
    CHECK(f.event == 0x0E);
    CHECK(f.len == 4);
    CHECK(memcmp(f.payload, want, 4) == 0);
}

static void test_garbage_resync(void)
{
    /* Line noise, then a valid frame: the noise is counted, the frame lands. */
    struct whci_parser p;
    struct whci_frame f;
    const uint8_t bytes[] = { 0x00, 0xFF, 0x7E, 0x19, 0x03, 0xF0, 0x00, 0x00 };
    whci_parser_init(&p);
    CHECK(feed(&p, bytes, sizeof(bytes), &f) == 1);
    CHECK(f.opcode == 0xF003);
    CHECK(p.garbage_bytes == 3);
}

static void test_back_to_back_frames(void)
{
    struct whci_parser p;
    struct whci_frame f;
    const uint8_t bytes[] = {
        0x19, 0x03, 0xF0, 0x00, 0x00,               /* PING */
        0x04, 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00,   /* H4 ack */
        0x19, 0x04, 0xF0, 0x01, 0x00, 0x01,         /* ADV on */
    };
    whci_parser_init(&p);
    CHECK(feed(&p, bytes, sizeof(bytes), &f) == 3);
    CHECK(f.kind == WHCI_PKT_WICED && f.opcode == 0xF004 && f.len == 1 && f.payload[0] == 1);
}

static void test_oversize_frame_dropped(void)
{
    /* A corrupt length beyond WHCI_MAX_PAYLOAD: the whole frame is swallowed
     * (no emit), counted, and the next frame still parses. */
    struct whci_parser p;
    struct whci_frame f;
    uint8_t bytes[5 + 300 + 5];
    size_t i = 0;
    bytes[i++] = 0x19; bytes[i++] = 0x01; bytes[i++] = 0xF0;
    bytes[i++] = (uint8_t)(300 & 0xFF); bytes[i++] = (uint8_t)(300 >> 8);
    for (int k = 0; k < 300; k++) bytes[i++] = 0x55;
    const uint8_t ping[] = { 0x19, 0x03, 0xF0, 0x00, 0x00 };
    memcpy(&bytes[i], ping, 5); i += 5;

    whci_parser_init(&p);
    CHECK(feed(&p, bytes, i, &f) == 1);
    CHECK(f.opcode == 0xF003);
    CHECK(p.dropped_frames == 1);
}

static void test_split_delivery(void)
{
    /* A frame split across arbitrary push boundaries parses identically. */
    struct whci_parser p;
    struct whci_frame f;
    uint8_t payload[] = { 1, 2, 3, 4, 5 };
    uint8_t buf[16];
    int n = whci_build(buf, sizeof(buf), 0xF001, payload, 5);
    whci_parser_init(&p);
    int frames = 0;
    frames += feed(&p, buf, 2, &f);
    frames += feed(&p, buf + 2, 1, &f);
    frames += feed(&p, buf + 3, (size_t)n - 3, &f);
    CHECK(frames == 1);
    CHECK(f.len == 5 && memcmp(f.payload, payload, 5) == 0);
}

int main(void)
{
    test_build_ping();
    test_build_adv_on();
    test_build_matches_captured_le_scan();
    test_build_too_small();
    test_parse_wiced_roundtrip();
    test_parse_zero_len_frame();
    test_parse_h4_event();
    test_garbage_resync();
    test_back_to_back_frames();
    test_oversize_frame_dropped();
    test_split_delivery();
    if (g_fail) {
        printf("test_wiced_hci: FAIL\n");
        return 1;
    }
    printf("test_wiced_hci: all passed\n");
    return 0;
}
