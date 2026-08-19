/*
 * bt_dump.c — read-only full-flash dump of the CYW20706 module (see bt_dump.h).
 *
 * Pure read: enters download mode (shared bt_wire) and loops READ_RAM over the
 * whole 512 KB serial flash at the ROM level — no minidriver, no WRITE_RAM, no
 * flash-write code compiled in. The raw bytes stream over the USB-CDC console
 * between DUMPSTART/DUMPEND markers; the nRF computes a CRC-32 the host verifies.
 *
 * Wire framing (host: scripts/dump_recv.py):
 *   [setup text lines...]
 *   "DUMPSTART <base8hex> <len_dec>\n"
 *   <len raw bytes>                     <- ONLY the flash contents
 *   "\nDUMPEND crc32=<8hex>\n"          <- stop signal + nRF checksum
 * The host reads exactly <len> bytes (length-delimited, not marker-delimited, so
 * a marker byte in the flash can't false-trigger), then compares its own CRC-32.
 */
#include "bt_dump.h"
#include "bt_wire.h"
#include "cybt_dl.h"
#include "usbdev.h"
#include "wdt.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/crc.h>
#include <hal/nrf_gpio.h>
#include "sp1_board.h"
#include <stdio.h>
#include <string.h>

/* Bump this whenever the dump firmware changes, so the console banner proves
 * which build is actually flashed (a stale .bin looks identical otherwise). */
#define DUMP_BUILD_TAG "pace16-noWQ"

/* The USB-CDC console UART. We write the raw stream to it directly with
 * uart_poll_out (synchronous, ordered) rather than printk (async), so the
 * binary can't be interleaved by a queued log line. */
static const struct device *con = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));

/* Time-based •• (SP1_FUNC_BTN, direct GPIO — needs no rail/controls) escape,
 * callable anywhere during the dump: hold •• ~1.5 s to power off. Time-based so
 * it triggers regardless of how often it's polled. */
static int64_t func_since = -1;
static void escape_check(void)
{
    if (nrf_gpio_pin_read(SP1_FUNC_BTN) == 0) {
        int64_t now = k_uptime_get();
        if (func_since < 0) {
            func_since = now;
        } else if (now - func_since >= 1500) {
            bt_wire_power_off();   /* never returns */
        }
    } else {
        func_since = -1;
    }
}

static void con_raw(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        /* Yield BEFORE each small block so the USB-CDC drains the TX ring before
         * we write more — poll_out must never hit a full ring (it blocks there
         * and the stream stalls ~512 B in). feed_wdt + •• escape ride along. */
        if ((i & 0x0F) == 0) {
            feed_wdt();
            escape_check();        /* •• hold powers off mid-stream */
            k_msleep(1);
        }
        uart_poll_out(con, b[i]);
    }
}

static void con_str(const char *s)
{
    con_raw((const uint8_t *)s, strlen(s));
}

void bt_dump_run(void)
{
    usbdev_start();
    for (int i = 0; i < 25; i++) {   /* WDT-fed USB settle */
        feed_wdt();
        k_msleep(100);
    }
    bt_wire_init();

    printk("\n=== sp1-beacon FLASH DUMP (read-only) build=%s ===\n", DUMP_BUILD_TAG);
    if (!bt_wire_enter_download()) {
        bt_wire_halt("DUMP: halted (download-mode entry failed).");
    }

    uint32_t base = CYBT_FLASH_BASE;                  /* 0xFF000000 */
    uint32_t len  = CYBT_FLASH_END - CYBT_FLASH_BASE; /* 0x80000 = 512 KB */

    /* Prompt on the same synchronous path as the stream, so ordering is exact. */
    con_str("\nDUMP: attach the receiver, then send any byte to begin "
            "(or it starts on its own in ~15 s).\n");

    /* Handshake: wait up to ~15 s for a host byte. A passive reader that's
     * already attached still catches the stream when the timeout fires. */
    {
        int64_t deadline = k_uptime_get() + 15000;
        uint8_t b;
        while (k_uptime_get() < deadline) {
            feed_wdt();
            if (uart_poll_in(con, &b) == 0) {
                break;
            }
            k_msleep(50);
        }
    }

    /* START marker. After this, ONLY flash bytes go out until DUMPEND — no
     * printk in this window (bt_wire_cmd_cc is silent on success; a failure
     * aborts the stream, which the host detects as a short read). */
    {
        char hdr[48];
        snprintf(hdr, sizeof(hdr), "\nDUMPSTART %08X %u\n", base, len);
        con_str(hdr);
    }

    /* •• is a direct GPIO (main configured it as input-pullup); make sure. */
    nrf_gpio_cfg_input(SP1_FUNC_BTN, NRF_GPIO_PIN_PULLUP);

    uint32_t crc = 0;
    bool ok = true;
    for (uint32_t off = 0; off < len; off += CYBT_READ_CHUNK) {
        feed_wdt();
        escape_check();   /* •• hold powers off between chunks too */
        uint8_t chunk = (len - off) < CYBT_READ_CHUNK ? (uint8_t)(len - off) : CYBT_READ_CHUNK;
        uint8_t cmd[16], data[CYBT_READ_CHUNK];
        uint16_t dlen = 0;
        int n = cybt_cmd_read_ram(cmd, sizeof(cmd), base + off, chunk);
        if (!bt_wire_cmd_cc(cmd, n, CYBT_OP_READ_RAM, data, sizeof(data), &dlen, 1000) || dlen != chunk) {
            /* Abort: the host sees fewer than <len> bytes and reports it. */
            con_str("\nDUMPABORT\n");
            printk("DUMP: READ_RAM failed at 0x%08X — aborted\n", base + off);
            ok = false;
            break;
        }
        crc = crc32_ieee_update(crc, data, chunk);
        con_raw(data, chunk);
        feed_wdt();
    }

    if (ok) {
        char tr[48];
        snprintf(tr, sizeof(tr), "\nDUMPEND crc32=%08X\n", crc);
        con_str(tr);
        printk("DUMP: complete — %u bytes, crc32=%08X\n", len, crc);
    }
    bt_wire_halt("DUMP: halted.");
}
