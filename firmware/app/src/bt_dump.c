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
#include <stdio.h>
#include <string.h>

/* The USB-CDC console UART. We write the raw stream to it directly with
 * uart_poll_out (synchronous, ordered) rather than printk (async), so the
 * binary can't be interleaved by a queued log line. */
static const struct device *con = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));

static void con_raw(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        /* Feed the WDT as we stream: if the host briefly stops draining, a
         * blocked poll_out shouldn't trip the ~8 s watchdog. (A fully-dead host
         * still eventually resets -> bootloop, which is recoverable; it can't
         * dead-end.) */
        if ((i & 0x3F) == 0) {
            feed_wdt();
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

    printk("\n=== sp1-beacon FLASH DUMP (read-only, no write path) ===\n");
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

    uint32_t crc = 0;
    bool ok = true;
    for (uint32_t off = 0; off < len; off += CYBT_READ_CHUNK) {
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
