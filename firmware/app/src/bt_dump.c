/*
 * bt_dump.c — read-only full-flash dump of the CYW20706 module (see bt_dump.h).
 *
 * Pure read: enters download mode (shared bt_wire) and loops READ_RAM over the
 * whole 512 KB serial flash at the ROM level — no minidriver, no WRITE_RAM, no
 * flash-write code compiled in.
 *
 * Delivery: each chunk is HEX-encoded and sent with printk, one line per chunk
 * (">" + 2 hex chars/byte). Raw uart_poll_out on the CDC only ever flushed the
 * initial ~512 B ring and then stopped (it doesn't re-kick the USB IN transfer);
 * printk goes through the console driver, which re-kicks every call and
 * sustains delivery — the same path that reliably carries all the status text.
 * The per-chunk module read (rx_frame, which yields) paces the output so printk
 * never floods. The nRF computes a CRC-32 over the raw bytes; the host decodes
 * the hex and re-checks. Wire framing (host: scripts/dump_recv.py):
 *   "DUMPSTART <base8hex> <len_dec>\n"
 *   ">FFA0..\n"  (one hex line per chunk, in order)   <- the flash contents
 *   "DUMPEND crc32=<8hex>\n"
 */
#include "bt_dump.h"
#include "bt_wire.h"
#include "cybt_dl.h"
#include "usbdev.h"
#include "wdt.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/crc.h>
#include <hal/nrf_gpio.h>
#include "sp1_board.h"

/* Bump on every dump-firmware change so the banner proves which build is live. */
#define DUMP_BUILD_TAG "hex-printk"

/* Time-based •• (SP1_FUNC_BTN, direct GPIO) escape: hold •• ~1.5 s to power off.
 * Polled between chunks; time-based so poll frequency doesn't matter. */
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

static void printk_hex_line(const uint8_t *d, uint8_t n)
{
    static const char H[] = "0123456789ABCDEF";
    char buf[2 * CYBT_READ_CHUNK + 1];
    for (uint8_t i = 0; i < n; i++) {
        buf[2 * i]     = H[d[i] >> 4];
        buf[2 * i + 1] = H[d[i] & 0x0F];
    }
    buf[2 * n] = '\0';
    printk(">%s\n", buf);
}

void bt_dump_run(void)
{
    usbdev_start();
    for (int i = 0; i < 30; i++) {   /* WDT-fed USB settle — start monitordump first */
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

    nrf_gpio_cfg_input(SP1_FUNC_BTN, NRF_GPIO_PIN_PULLUP);   /* •• escape */

    printk("DUMPSTART %08X %u\n", base, len);

    uint32_t crc = 0;
    bool ok = true;
    int resyncs = 0;                     /* total download-mode re-entries */
    for (uint32_t off = 0; off < len; off += CYBT_READ_CHUNK) {
        feed_wdt();
        escape_check();   /* •• hold powers off between chunks */
        uint8_t chunk = (len - off) < CYBT_READ_CHUNK ? (uint8_t)(len - off) : CYBT_READ_CHUNK;
        uint8_t cmd[16], data[CYBT_READ_CHUNK];
        uint16_t dlen = 0;
        int n = cybt_cmd_read_ram(cmd, sizeof(cmd), base + off, chunk);

        /* The module has gone silent mid-dump, reproducibly at one address.
         * Tell a recoverable dropout apart from a hard ROM read boundary: on a
         * failed read, reset the module back into download mode
         * (bt_wire_enter_download = RST + flush + autobaud, all read-only) and
         * retry the SAME chunk. Recovers -> it was a dropout (session timeout);
         * still dead after a fresh reset -> genuine boundary. Never writes. */
        int tries = 0;
        while (!bt_wire_cmd_cc(cmd, n, CYBT_OP_READ_RAM, data, sizeof(data), &dlen, 1000) || dlen != chunk) {
            feed_wdt();
            escape_check();
            if (++tries > 5) {
                printk("DUMPABORT at 0x%08X (unrecovered after %d resyncs)\n", base + off, resyncs);
                ok = false;
                break;
            }
            printk("DL: read stalled at 0x%08X — re-entering download mode (retry %d)\n", base + off, tries);
            resyncs++;
            bt_wire_enter_download();     /* RST + flush + autobaud; read-only */
            dlen = 0;
        }
        if (!ok) {
            break;
        }
        crc = crc32_ieee_update(crc, data, chunk);
        printk_hex_line(data, chunk);   /* re-kicks the USB transfer every line */
    }

    if (ok) {
        printk("DUMPEND crc32=%08X\n", crc);
        printk("DUMP: complete — %u bytes, crc32=%08X\n", len, crc);
    }
    bt_wire_halt("DUMP: halted.");
}
