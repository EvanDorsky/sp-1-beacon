/*
 * bt_download.c — dev-only CYW20706 module flasher (see bt_download.h).
 *
 * Drives the download ROM over uart0 in POLL mode with the download strap
 * (module CTS held LOW across the reset release — the opposite of the normal
 * runtime link). Uses the host-tested pure core in cybt_dl.c for every command
 * byte and every safety gate; this file is only the I/O + sequencing shell.
 *
 * NOT HARDWARE-VALIDATED. The pure protocol/planning core is host-tested, but
 * the on-wire sequence (autobaud timing, flow control, minidriver launch)
 * needs bench bring-up. Run the dry-run build first; the armed write is gated
 * behind CONFIG_SP1_BT_DOWNLOAD_ARM. Read bluetooth/reflashing-the-module.md
 * before running on hardware.
 */
#include "bt_download.h"
#include "cybt_dl.h"
#include "wiced_hci.h"
#include "usbdev.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_gpio.h>

#include "cybt_blobs.h"   /* generated, gitignored: cybt_minidriver[], cybt_ds_image[], addrs */

#define MODULE_RSTN  NRF_GPIO_PIN_MAP(0, 10)   /* RST_N, active low */
#define MODULE_CTS   NRF_GPIO_PIN_MAP(1, 1)    /* module CTS <- our GPIO; LOW = download strap */

static const struct device *uart = DEVICE_DT_GET(DT_NODELABEL(uart0));

static struct whci_parser parser;

/* ---- interrupt-driven RX ring (poll-mode UARTE drops bytes on the long
 * READ_RAM responses; buffer in an ISR like module_link does). ---- */
#define RX_RING 1024
static uint8_t  ring[RX_RING];
static volatile uint32_t ring_head, ring_tail, ring_overruns, rx_total;

static void dl_uart_isr(const struct device *dev, void *user_data)
{
    uint8_t buf[64];
    (void)user_data;
    while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
        int n = uart_fifo_read(dev, buf, sizeof(buf));
        for (int i = 0; i < n; i++) {
            uint32_t head = ring_head;
            if (head - ring_tail >= RX_RING) {
                ring_overruns++;
                continue;
            }
            ring[head % RX_RING] = buf[i];
            ring_head = head + 1;
            rx_total++;
        }
        if (n <= 0) {
            break;
        }
    }
}

static void rx_init(void)
{
    ring_head = ring_tail = ring_overruns = rx_total = 0;
    if (!device_is_ready(uart)) {
        printk("DL: uart0 NOT READY\n");
        return;
    }
    uart_irq_callback_user_data_set(uart, dl_uart_isr, NULL);
    uart_irq_rx_enable(uart);
}

static void tx(const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) {
        uart_poll_out(uart, b[i]);
    }
}

/* Pull the next complete frame from the ring through the codec, or false on
 * timeout (ms). */
static bool rx_frame(struct whci_frame *out, int timeout_ms)
{
    int64_t deadline = k_uptime_get() + timeout_ms;
    while (k_uptime_get() < deadline) {
        while (ring_tail != ring_head) {
            uint8_t byte = ring[ring_tail % RX_RING];
            ring_tail++;
            if (whci_parse_byte(&parser, byte, out)) {
                return true;
            }
        }
        k_busy_wait(100);
    }
    return false;
}

/* Send a command, wait for its command-complete ack; on success copy up to
 * cap bytes of ack data into data/*dlen. Returns true on a status-0 ack.
 * On failure, logs every frame it DID see (so a bad status / wrong opcode /
 * short read is visible on the console). */
static bool cmd_cc(const uint8_t *cmd, int clen, uint16_t opcode,
                   uint8_t *data, uint16_t cap, uint16_t *dlen, int timeout_ms)
{
    struct whci_frame f;
    uint32_t rx_before = rx_total;
    int seen = 0;

    tx(cmd, clen);
    while (rx_frame(&f, timeout_ms)) {
        seen++;
        if (f.kind != WHCI_PKT_HCI_EVT || f.event != 0x0E) {
            printk("DL:   rx non-CC frame kind=%02x evt=%02x len=%u\n",
                   f.kind, f.event, f.len);
            continue;
        }
        const uint8_t *d;
        uint16_t dl;
        if (cybt_cc_ok(f.payload, f.len, opcode, &d, &dl)) {
            if (data && dlen) {
                uint16_t copy = dl < cap ? dl : cap;
                for (uint16_t i = 0; i < copy; i++) {
                    data[i] = d[i];
                }
                *dlen = copy;
            }
            return true;
        }
        /* A command-complete, but not the one we wanted (wrong opcode or
         * nonzero status): show it — this is the key diagnostic. */
        printk("DL:   rx CC op=%02x%02x status=%02x (wanted op=%04x) len=%u\n",
               f.payload[2], f.payload[1], f.len >= 4 ? f.payload[3] : 0xFF,
               opcode, f.len);
    }
    printk("DL:   (no matching ack: %d frame(s) seen, %u rx bytes, %u overruns)\n",
           seen, rx_total - rx_before, ring_overruns);
    return false;
}

/* ---- download-mode entry (Recovery Reset + autobaud HCI_RESET loop) ---- */

static bool enter_download(void)
{
    uint8_t cmd[16];
    int n = cybt_cmd_hci_reset(cmd, sizeof(cmd));

    /* Strap: module CTS LOW across the reset release selects download mode. */
    nrf_gpio_pin_clear(MODULE_CTS);
    nrf_gpio_cfg_output(MODULE_CTS);
    nrf_gpio_pin_clear(MODULE_RSTN);
    nrf_gpio_cfg_output(MODULE_RSTN);
    k_msleep(10);
    nrf_gpio_pin_set(MODULE_RSTN);      /* release reset with CTS still LOW */
    k_msleep(10);

    ring_tail = ring_head;              /* flush any pre-download boot chatter */
    whci_parser_init(&parser);
    /* The ROM re-autobauds on entry: the first one or two HCI_RESETs get
     * dropped. Loop until the ack lands (doc §3/§5). */
    for (int attempt = 0; attempt < 20; attempt++) {
        if (cmd_cc(cmd, n, CYBT_OP_HCI_RESET, NULL, 0, NULL, 200)) {
            printk("DL: download mode entered (HCI_RESET ack on attempt %d)\n", attempt + 1);
            return true;
        }
    }
    printk("DL: FAILED to enter download mode (no HCI_RESET ack)\n");
    return false;
}

/* ---- SS identity gate (ROM-level READ_RAM, no minidriver) ---- */

#define SS_LEN 64

/* Flush any buffered RX + reset the frame parser to a clean slate. Call before
 * each request/response exchange so stale bytes can't mis-frame the reply. */
static void rx_flush(void)
{
    ring_tail = ring_head;
    whci_parser_init(&parser);
}

/* Diagnostic: send one READ_RAM and dump the RAW response bytes (read-only,
 * no framing assumptions) so we can see exactly what the ROM returns. */
static void diag_raw_read(uint32_t addr, uint8_t len)
{
    uint8_t cmd[16];
    int n = cybt_cmd_read_ram(cmd, sizeof(cmd), addr, len);
    int64_t deadline;
    int count = 0;

    rx_flush();
    printk("DL: [diag] READ_RAM 0x%08X len %u -> tx:", addr, len);
    for (int i = 0; i < n; i++) {
        printk(" %02x", cmd[i]);
    }
    printk("\nDL: [diag] raw rx:");
    tx(cmd, n);
    deadline = k_uptime_get() + 800;
    while (k_uptime_get() < deadline) {
        while (ring_tail != ring_head) {
            uint8_t b = ring[ring_tail % RX_RING];
            ring_tail++;
            printk(" %02x", b);
            count++;
        }
        k_busy_wait(100);
    }
    printk("\nDL: [diag] %d raw bytes, %u overruns\n", count, ring_overruns);
}

static bool read_ss(uint8_t *out /* SS_LEN */)
{
    rx_flush();   /* clean slate: no stale bytes from the entry handshake */
    /* Read the SS in READ_CHUNK slices at ROM level. */
    for (uint32_t off = 0; off < SS_LEN; off += CYBT_READ_CHUNK) {
        uint8_t chunk = (SS_LEN - off) < CYBT_READ_CHUNK ? (uint8_t)(SS_LEN - off) : CYBT_READ_CHUNK;
        uint8_t cmd[16];
        uint8_t data[CYBT_READ_CHUNK];
        uint16_t dlen = 0;
        int n = cybt_cmd_read_ram(cmd, sizeof(cmd), CYBT_FLASH_BASE + off, chunk);
        if (!cmd_cc(cmd, n, CYBT_OP_READ_RAM, data, sizeof(data), &dlen, 500) || dlen != chunk) {
            return false;
        }
        for (uint16_t i = 0; i < dlen; i++) {
            out[off + i] = data[i];
        }
    }
    return true;
}

static bool identity_gate(void)
{
    uint8_t ss1[SS_LEN], ss2[SS_LEN];
    uint32_t base;

    /* Diagnostic first: dump the raw wire bytes for a small SS read + a RAM
     * read (a known-good control address) so we can see what the ROM returns. */
    diag_raw_read(CYBT_FLASH_BASE, 16);   /* SS @ 0xFF000000 */
    diag_raw_read(0x00200000u, 16);       /* on-chip RAM base — control read */

    /* Flash reads are deterministic; require two byte-identical reads so a
     * flaky read can't spoof the gate (findings.md). */
    if (!read_ss(ss1) || !read_ss(ss2)) {
        printk("DL: SS read failed\n");
        return false;
    }
    for (int i = 0; i < SS_LEN; i++) {
        if (ss1[i] != ss2[i]) {
            printk("DL: SS reads differ at %d — ABORT\n", i);
            return false;
        }
    }
    if (!cybt_ss_ds_base(ss1, SS_LEN, &base)) {
        printk("DL: SS has no DS-base record — ABORT\n");
        return false;
    }
    printk("DL: SS DS-base = 0x%08X (expect 0x%08X)\n", base, CYBT_DS_BASE);
    /* BD_ADDR appears at SS data offset 21 (doc §3); print a few bytes as an
     * eyeball identity aid (not reproduced elsewhere). */
    printk("DL: SS[18..23] = %02X %02X %02X %02X %02X %02X\n",
           ss1[18], ss1[19], ss1[20], ss1[21], ss1[22], ss1[23]);
    if (!cybt_ss_gate_ok(ss1, SS_LEN)) {
        printk("DL: IDENTITY GATE FAILED (DS base != expected) — ABORT, no write\n");
        return false;
    }
    printk("DL: identity gate OK\n");
    return true;
}

/* ---- minidriver load ---- */

static bool load_minidriver(void)
{
    uint8_t cmd[16];
    int n = cybt_cmd_download_minidriver(cmd, sizeof(cmd));
    if (!cmd_cc(cmd, n, CYBT_OP_DL_MINIDRIVER, NULL, 0, NULL, 500)) {
        printk("DL: DOWNLOAD_MINIDRIVER announce not acked\n");
        return false;
    }
    uint32_t total = sizeof(cybt_minidriver);
    /* Defense in depth: the minidriver loads into RAM, never flash. Refuse if a
     * corrupt blob address ever pointed this WRITE_RAM at the flash window — the
     * DS floor guards the DS write, and this guards the only other WRITE_RAM. */
    if (CYBT_BLOB_MINIDRIVER_ADDR >= CYBT_FLASH_BASE ||
        (uint64_t)CYBT_BLOB_MINIDRIVER_ADDR + total > CYBT_FLASH_BASE) {
        printk("DL: ABORT — minidriver addr 0x%08X is in flash space, not RAM\n",
               (unsigned)CYBT_BLOB_MINIDRIVER_ADDR);
        return false;
    }
    for (uint32_t off = 0; off < total; off += CYBT_WRITE_CHUNK) {
        uint8_t chunk = (total - off) < CYBT_WRITE_CHUNK ? (uint8_t)(total - off) : CYBT_WRITE_CHUNK;
        uint8_t wcmd[8 + CYBT_WRITE_CHUNK];
        int wn = cybt_cmd_write_ram(wcmd, sizeof(wcmd),
                                    CYBT_BLOB_MINIDRIVER_ADDR + off, &cybt_minidriver[off], chunk);
        if (wn < 0 || !cmd_cc(wcmd, wn, CYBT_OP_WRITE_RAM, NULL, 0, NULL, 500)) {
            printk("DL: minidriver WRITE_RAM failed at +0x%X\n", off);
            return false;
        }
    }
    int ln = cybt_cmd_launch_ram(cmd, sizeof(cmd), CYBT_BLOB_MINIDRIVER_LAUNCH);
    if (!cmd_cc(cmd, ln, CYBT_OP_LAUNCH_RAM, NULL, 0, NULL, 500)) {
        printk("DL: minidriver LAUNCH_RAM not acked\n");
        return false;
    }
    printk("DL: minidriver launched (%u bytes @ 0x%08X, launch 0x%08X)\n",
           total, CYBT_BLOB_MINIDRIVER_ADDR, CYBT_BLOB_MINIDRIVER_LAUNCH);
    return true;
}

/* ---- DS plan (dry-run) + write (armed) ---- */

static bool ds_plan(void)
{
    uint32_t total = sizeof(cybt_ds_image);
    uint32_t base = CYBT_BLOB_DS_ADDR;
    uint32_t nchunks = (total + CYBT_WRITE_CHUNK - 1) / CYBT_WRITE_CHUNK;

    if (base != CYBT_DS_BASE) {
        printk("DL: blob DS addr 0x%08X != CYBT_DS_BASE 0x%08X — ABORT\n", base, CYBT_DS_BASE);
        return false;
    }
    /* Every chunk must be inside the DS window (the load-bearing floor check). */
    for (uint32_t off = 0; off < total; off += CYBT_WRITE_CHUNK) {
        uint32_t len = (total - off) < CYBT_WRITE_CHUNK ? (total - off) : CYBT_WRITE_CHUNK;
        if (!cybt_addr_in_ds_window(base + off, len)) {
            printk("DL: chunk @0x%08X len %u OUTSIDE DS window — ABORT\n", base + off, len);
            return false;
        }
    }
    printk("DL: DS plan OK — %u bytes, %u chunks, 0x%08X..0x%08X\n",
           total, nchunks, base, base + total);
    return true;
}

#ifdef CONFIG_SP1_BT_DOWNLOAD_ARM
static bool ds_write_and_verify(void)
{
    uint32_t total = sizeof(cybt_ds_image);
    uint32_t base = CYBT_BLOB_DS_ADDR;

    for (uint32_t off = 0; off < total; off += CYBT_WRITE_CHUNK) {
        uint32_t len = (total - off) < CYBT_WRITE_CHUNK ? (total - off) : CYBT_WRITE_CHUNK;
        /* Re-check EACH chunk against the floor right before emitting it. */
        if (!cybt_addr_in_ds_window(base + off, len)) {
            printk("DL: ABORT — chunk @0x%08X failed the floor check at write time\n", base + off);
            return false;
        }
        uint8_t wcmd[8 + CYBT_WRITE_CHUNK];
        int wn = cybt_cmd_write_ram(wcmd, sizeof(wcmd), base + off, &cybt_ds_image[off], (uint8_t)len);
        if (wn < 0 || !cmd_cc(wcmd, wn, CYBT_OP_WRITE_RAM, NULL, 0, NULL, 1000)) {
            printk("DL: DS WRITE_RAM failed at +0x%X\n", off);
            return false;
        }
        if ((off % 4096) == 0) {
            printk("DL: wrote 0x%08X (%u/%u)\n", base + off, off, total);
        }
    }
    printk("DL: DS write complete, verifying by read-back...\n");
    /* Read-back verify: byte-for-byte compare (findings.md's proof method). */
    for (uint32_t off = 0; off < total; off += CYBT_READ_CHUNK) {
        uint8_t chunk = (total - off) < CYBT_READ_CHUNK ? (uint8_t)(total - off) : CYBT_READ_CHUNK;
        uint8_t cmd[16], rd[CYBT_READ_CHUNK];
        uint16_t dlen = 0;
        int n = cybt_cmd_read_ram(cmd, sizeof(cmd), base + off, chunk);
        if (!cmd_cc(cmd, n, CYBT_OP_READ_RAM, rd, sizeof(rd), &dlen, 1000) || dlen != chunk) {
            printk("DL: read-back failed at +0x%X\n", off);
            return false;
        }
        for (uint16_t i = 0; i < chunk; i++) {
            if (rd[i] != cybt_ds_image[off + i]) {
                printk("DL: read-back MISMATCH at 0x%08X — ABORT\n", base + off + i);
                return false;
            }
        }
    }
    printk("DL: read-back MATCH\n");
    return true;
}
#endif /* CONFIG_SP1_BT_DOWNLOAD_ARM */

void bt_download_run(void)
{
    /* Bring up the USB-CDC console (this build has no SWD/RTT) and give a host
     * a couple of seconds to attach before the log starts. */
    usbdev_start();
    k_msleep(2500);
    rx_init();      /* interrupt-driven RX before any UART traffic */

#ifdef CONFIG_SP1_BT_DOWNLOAD_ARM
    printk("\n=== sp1-beacon MODULE FLASHER — ARMED (will WRITE the DS) ===\n");
#else
    printk("\n=== sp1-beacon MODULE FLASHER — DRY-RUN (no write) ===\n");
#endif
    printk("DS base 0x%08X, DS floor 0x%08X. No chip-erase compiled.\n",
           CYBT_DS_BASE, CYBT_DS_FLOOR);

    if (!enter_download()) {
        goto halt;
    }
    /* Identity gate BEFORE the minidriver (findings.md): a mismatch aborts
     * before any write machinery is even loaded. */
    if (!identity_gate()) {
        goto halt;
    }
    if (!load_minidriver()) {
        goto halt;
    }
    if (!ds_plan()) {
        goto halt;
    }

#ifdef CONFIG_SP1_BT_DOWNLOAD_ARM
    printk("DL: ARMED — writing DS now.\n");
    if (!ds_write_and_verify()) {
        goto halt;
    }
    /* SS must survive byte-identical. */
    {
        uint8_t ss[SS_LEN];
        uint32_t base;
        if (read_ss(ss) && cybt_ss_ds_base(ss, SS_LEN, &base) && base == CYBT_DS_BASE) {
            printk("DL: SS intact after write (DS base still 0x%08X)\n", base);
        } else {
            printk("DL: WARNING — SS re-check failed after write\n");
        }
    }
    /* Warm-boot into the freshly written app. */
    {
        uint8_t cmd[16];
        int n = cybt_cmd_launch_ram(cmd, sizeof(cmd), CYBT_LAUNCH_REBOOT);
        tx(cmd, n);
    }
    printk("DL: DONE — module warm-booted into the new app.\n");
#else
    printk("DL: DRY-RUN complete — identity + plan OK, nothing written.\n");
    printk("DL: build with CONFIG_SP1_BT_DOWNLOAD_ARM=y to perform the write.\n");
#endif

halt:
    printk("DL: halted. Power-cycle to boot the module's current app.\n");
    for (;;) {
        k_msleep(1000);
    }
}
