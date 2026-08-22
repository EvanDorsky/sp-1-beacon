/*
 * Host test for the pure CYW20706 download-protocol core (cybt_dl.c).
 *
 * Byte-for-byte against the opcodes in bluetooth/hardware-and-architecture.md
 * §7, and hammering the safety guards (DS-window floor, SS identity gate)
 * since a bug there could brick an irreplaceable radio.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cybt_dl.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static void test_cmd_hci_reset(void)
{
    uint8_t b[8];
    int n = cybt_cmd_hci_reset(b, sizeof(b));
    const uint8_t want[] = { 0x01, 0x03, 0x0C, 0x00 };
    CHECK(n == 4 && memcmp(b, want, 4) == 0);
}

static void test_cmd_download_minidriver(void)
{
    uint8_t b[8];
    int n = cybt_cmd_download_minidriver(b, sizeof(b));
    const uint8_t want[] = { 0x01, 0x2E, 0xFC, 0x00 };
    CHECK(n == 4 && memcmp(b, want, 4) == 0);
}

static void test_cmd_write_ram(void)
{
    uint8_t b[16];
    uint8_t data[] = { 0xAA, 0xBB, 0xCC };
    /* addr 0x00220000, 3 data bytes -> plen = 4+3 = 7 */
    int n = cybt_cmd_write_ram(b, sizeof(b), 0x00220000u, data, 3);
    const uint8_t want[] = { 0x01, 0x4C, 0xFC, 0x07, 0x00, 0x00, 0x22, 0x00, 0xAA, 0xBB, 0xCC };
    CHECK(n == 11 && memcmp(b, want, 11) == 0);
}

static void test_cmd_read_ram(void)
{
    uint8_t b[16];
    /* READ_RAM 0xFF000000 len 64 -> 01 4D FC 05 00 00 00 FF 40 */
    int n = cybt_cmd_read_ram(b, sizeof(b), 0xFF000000u, 64);
    const uint8_t want[] = { 0x01, 0x4D, 0xFC, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x40 };
    CHECK(n == 9 && memcmp(b, want, 9) == 0);
}

static void test_cmd_launch_ram(void)
{
    uint8_t b[16];
    int n = cybt_cmd_launch_ram(b, sizeof(b), CYBT_LAUNCH_REBOOT);
    const uint8_t want[] = { 0x01, 0x4E, 0xFC, 0x04, 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK(n == 8 && memcmp(b, want, 8) == 0);
    /* and the minidriver launch address */
    n = cybt_cmd_launch_ram(b, sizeof(b), CYBT_MINIDRIVER_RAM);
    const uint8_t want2[] = { 0x01, 0x4E, 0xFC, 0x04, 0x00, 0x00, 0x22, 0x00 };
    CHECK(n == 8 && memcmp(b, want2, 8) == 0);
}

static void test_cmd_too_small(void)
{
    uint8_t b[3];
    CHECK(cybt_cmd_hci_reset(b, sizeof(b)) == -1);
}

static void test_cc_ok(void)
{
    /* WRITE_RAM ack event 04 0E 04 01 4C FC 00 -> payload after 04 0E len is
     * 01 4C FC 00 */
    const uint8_t pl[] = { 0x01, 0x4C, 0xFC, 0x00 };
    const uint8_t *data = NULL;
    uint16_t dlen = 99;
    CHECK(cybt_cc_ok(pl, sizeof(pl), CYBT_OP_WRITE_RAM, &data, &dlen));
    CHECK(dlen == 0);
    /* wrong opcode */
    CHECK(!cybt_cc_ok(pl, sizeof(pl), CYBT_OP_READ_RAM, NULL, NULL));
    /* nonzero status */
    const uint8_t bad[] = { 0x01, 0x4C, 0xFC, 0x01 };
    CHECK(!cybt_cc_ok(bad, sizeof(bad), CYBT_OP_WRITE_RAM, NULL, NULL));
}

static void test_cc_ok_read_data(void)
{
    /* READ_RAM ack with 4 data bytes: 01 4D FC 00 <d0 d1 d2 d3> */
    const uint8_t pl[] = { 0x01, 0x4D, 0xFC, 0x00, 0xDE, 0xAD, 0xBE, 0xEF };
    const uint8_t *data = NULL;
    uint16_t dlen = 0;
    CHECK(cybt_cc_ok(pl, sizeof(pl), CYBT_OP_READ_RAM, &data, &dlen));
    CHECK(dlen == 4 && data[0] == 0xDE && data[3] == 0xEF);
}

static void test_ds_window_guard(void)
{
    /* At the floor: allowed. */
    CHECK(cybt_addr_in_ds_window(CYBT_DS_FLOOR, 240));
    /* Below the floor (toward VS/SS): refused — the load-bearing check. */
    CHECK(!cybt_addr_in_ds_window(CYBT_DS_FLOOR - 1, 1));
    CHECK(!cybt_addr_in_ds_window(CYBT_FLASH_BASE, 1));      /* the SS itself */
    CHECK(!cybt_addr_in_ds_window(CYBT_VS_END - 0x1000, 240)); /* VS region */
    /* Past the end of flash: refused. */
    CHECK(!cybt_addr_in_ds_window(CYBT_FLASH_END - 100, 240));
    CHECK(cybt_addr_in_ds_window(CYBT_FLASH_END - 240, 240)); /* exactly fits */
    /* Zero length and wrap: refused. */
    CHECK(!cybt_addr_in_ds_window(CYBT_DS_FLOOR, 0));
    CHECK(!cybt_addr_in_ds_window(0xFFFFFF00u, 0x200));
}

/* Build an SS TLV: a couple of records then a type-0x02 with a DS-base pointer,
 * then 0xFF padding. */
static uint32_t build_ss(uint8_t *ss, uint32_t ds_base, int include_type2)
{
    uint32_t o = 0;
    /* type 0x01, len 3 */
    ss[o++] = 0x01; ss[o++] = 0x03; ss[o++] = 0x00; ss[o++] = 0x11; ss[o++] = 0x22; ss[o++] = 0x33;
    if (include_type2) {
        /* type 0x02, len 4 = the DS base LE32 */
        ss[o++] = 0x02; ss[o++] = 0x04; ss[o++] = 0x00;
        ss[o++] = (uint8_t)(ds_base);
        ss[o++] = (uint8_t)(ds_base >> 8);
        ss[o++] = (uint8_t)(ds_base >> 16);
        ss[o++] = (uint8_t)(ds_base >> 24);
    }
    /* 0xFF padding to 64 bytes */
    while (o < 64) ss[o++] = 0xFF;
    return o;
}

static void test_ss_ds_base(void)
{
    uint8_t ss[64];
    uint32_t base = 0;

    /* Hardware encoding: the SS stores a flash OFFSET (0x3000 on the SP-1);
     * the parser normalizes it to the mapped address 0xFF003000. */
    uint32_t len = build_ss(ss, 0x00003000u, 1);
    CHECK(cybt_ss_ds_base(ss, len, &base));
    CHECK(base == 0xFF003000u);

    /* Already-mapped encoding passes through unchanged (idempotent). */
    len = build_ss(ss, 0xFF003000u, 1);
    CHECK(cybt_ss_ds_base(ss, len, &base) && base == 0xFF003000u);

    /* eval-board layout (offset 0x4000 -> 0xFF004000) parses fine but must NOT
     * pass the gate for an SP-1 build. */
    len = build_ss(ss, 0x00004000u, 1);
    CHECK(cybt_ss_ds_base(ss, len, &base) && base == 0xFF004000u);
    CHECK(!cybt_ss_gate_ok(ss, len));

    /* No type-0x02 record: refuse. */
    len = build_ss(ss, 0x00003000u, 0);
    CHECK(!cybt_ss_ds_base(ss, len, &base));
    CHECK(!cybt_ss_gate_ok(ss, len));
}

static void test_ss_gate(void)
{
    uint8_t ss[64];
    uint32_t len = build_ss(ss, 0x00003000u, 1);   /* SP-1 offset -> 0xFF003000 */
    CHECK(cybt_ss_gate_ok(ss, len));               /* matching base: OK */

    /* Malformed streams degrade to a safe refuse, never a crash/redirect. */
    uint8_t junk[64];
    memset(junk, 0xFF, sizeof(junk));
    CHECK(!cybt_ss_gate_ok(junk, sizeof(junk)));   /* all padding */
    uint8_t truncated[] = { 0x02, 0x04, 0x00, 0x00 };  /* claims 4 but has 1 */
    CHECK(!cybt_ss_gate_ok(truncated, sizeof(truncated)));
    CHECK(!cybt_ss_gate_ok(NULL, 0));
    /* A record whose length runs past the buffer: refuse. */
    uint8_t overrun[] = { 0x01, 0xFF, 0x00, 0x00 };
    CHECK(!cybt_ss_gate_ok(overrun, sizeof(overrun)));
}

/* Template-equality gate: byte-identical except the 6 BD_ADDR bytes at 21. */
static void test_ss_template(void)
{
    uint8_t tmpl[64], ss[64];
    for (int i = 0; i < 64; i++) {
        tmpl[i] = (uint8_t)(i * 7u + 3u);
    }
    memset(tmpl + CYBT_SS_BDADDR_OFF, 0x00, CYBT_SS_BDADDR_LEN);  /* masked in template */

    memcpy(ss, tmpl, 64);
    CHECK(cybt_ss_template_ok(ss, 64, tmpl, 64));                 /* identical passes */

    memcpy(ss + CYBT_SS_BDADDR_OFF, "\xC0\x5D\x89\x12\x34\x56", 6);
    CHECK(cybt_ss_template_ok(ss, 64, tmpl, 64));                 /* BD_ADDR free */

    ss[0] ^= 1;
    CHECK(!cybt_ss_template_ok(ss, 64, tmpl, 64));                /* header byte -> refuse */
    ss[0] ^= 1;

    ss[63] ^= 1;
    CHECK(!cybt_ss_template_ok(ss, 64, tmpl, 64));                /* trailing byte -> refuse */
    ss[63] ^= 1;

    ss[CYBT_SS_BDADDR_OFF + CYBT_SS_BDADDR_LEN] ^= 1;             /* just past the mask */
    CHECK(!cybt_ss_template_ok(ss, 64, tmpl, 64));
    ss[CYBT_SS_BDADDR_OFF + CYBT_SS_BDADDR_LEN] ^= 1;

    ss[CYBT_SS_BDADDR_OFF - 1] ^= 1;                              /* just before the mask */
    CHECK(!cybt_ss_template_ok(ss, 64, tmpl, 64));
    ss[CYBT_SS_BDADDR_OFF - 1] ^= 1;

    CHECK(!cybt_ss_template_ok(ss, 32, tmpl, 64));                /* short SS refused */
    CHECK(!cybt_ss_template_ok(NULL, 64, tmpl, 64));
    CHECK(!cybt_ss_template_ok(ss, 64, NULL, 64));
    CHECK(!cybt_ss_template_ok(ss, 64, tmpl, 0));
}

int main(void)
{
    test_cmd_hci_reset();
    test_cmd_download_minidriver();
    test_cmd_write_ram();
    test_cmd_read_ram();
    test_cmd_launch_ram();
    test_cmd_too_small();
    test_cc_ok();
    test_cc_ok_read_data();
    test_ds_window_guard();
    test_ss_ds_base();
    test_ss_gate();
    test_ss_template();
    if (g_fail) {
        printf("test_cybt_dl: FAIL\n");
        return 1;
    }
    printf("test_cybt_dl: all passed\n");
    return 0;
}
