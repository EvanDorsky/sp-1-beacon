/*
 * bt_download.c — CYW20706 module flashing engine (see bt_download.h).
 *
 * One engine, two entries: the dev boot-mode flasher (CONFIG_SP1_BT_DOWNLOAD,
 * write armed by CONFIG_SP1_BT_DOWNLOAD_ARM) and release on-device
 * provisioning (CONFIG_SP1_PROVISION, bt_provision_run). Identity + SS
 * template gate, minidriver load, DS plan, DS write + read-back verify, SS
 * re-check, warm boot. The low-level UART / download-mode entry / power-off
 * recovery is shared with the read-only dumper via bt_wire.c.
 *
 * The armed write is HARDWARE-VALIDATED (two DS writes on the keeper, SS
 * byte-identical both times). Read bluetooth/reflashing-the-module.md first.
 */
#include "bt_download.h"
#include "bt_wire.h"
#include "cybt_dl.h"
#include "usbdev.h"
#include "wdt.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "cybt_blobs.h"   /* generated, gitignored: cybt_minidriver[], cybt_ds_image[], addrs */

#if defined(CONFIG_SP1_PROVISION) && !defined(CYBT_HAVE_SS_TEMPLATE)
#error "CONFIG_SP1_PROVISION requires the SS identity template — regenerate cybt_blobs.h with gen_blobs.py --ss-template <known-good flash dump>"
#endif

/* Progress reporting for the provisioning caller's LED display (NULL for the
 * dev boot mode, which narrates over the console instead). */
static bt_prov_progress_t prov_cb;
static void report(enum bt_prov_phase phase, int pct)
{
    if (prov_cb != NULL) {
        prov_cb(phase, pct);
    }
}

/* ---- SS identity gate (ROM-level READ_RAM, no minidriver) ---- */

#define SS_LEN 64

static bool read_ss(uint8_t *out /* SS_LEN */)
{
    bt_wire_flush();   /* clean slate: no stale bytes from the entry handshake */
    for (uint32_t off = 0; off < SS_LEN; off += CYBT_READ_CHUNK) {
        uint8_t chunk = (SS_LEN - off) < CYBT_READ_CHUNK ? (uint8_t)(SS_LEN - off) : CYBT_READ_CHUNK;
        uint8_t cmd[16];
        uint8_t data[CYBT_READ_CHUNK];
        uint16_t dlen = 0;
        int n = cybt_cmd_read_ram(cmd, sizeof(cmd), CYBT_FLASH_BASE + off, chunk);
        if (!bt_wire_cmd_cc(cmd, n, CYBT_OP_READ_RAM, data, sizeof(data), &dlen, 500) || dlen != chunk) {
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
#ifdef CYBT_HAVE_SS_TEMPLATE
    /* feldd's template-equality gate: the whole SS window must match the
     * known-good factory template except the 6 BD_ADDR bytes. Any deviation
     * refuses — never guess, never relocate. */
    if (!cybt_ss_template_ok(ss1, SS_LEN, cybt_ss_template,
                             sizeof(cybt_ss_template))) {
        printk("DL: SS does not match the factory template — ABORT, no write\n");
        return false;
    }
    printk("DL: SS template match (BD_ADDR-masked)\n");
#endif
    printk("DL: identity gate OK\n");
    return true;
}

/* ---- minidriver load ---- */

static bool load_minidriver(void)
{
    uint8_t cmd[16];
    int n = cybt_cmd_download_minidriver(cmd, sizeof(cmd));
    if (!bt_wire_cmd_cc(cmd, n, CYBT_OP_DL_MINIDRIVER, NULL, 0, NULL, 500)) {
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
        if (wn < 0 || !bt_wire_cmd_cc(wcmd, wn, CYBT_OP_WRITE_RAM, NULL, 0, NULL, 500)) {
            printk("DL: minidriver WRITE_RAM failed at +0x%X\n", off);
            return false;
        }
        report(BT_PROV_PREP, (int)(20 + off * 80 / total));   /* prep 20..100 */
    }
    int ln = cybt_cmd_launch_ram(cmd, sizeof(cmd), CYBT_BLOB_MINIDRIVER_LAUNCH);
    if (!bt_wire_cmd_cc(cmd, ln, CYBT_OP_LAUNCH_RAM, NULL, 0, NULL, 500)) {
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

#if defined(CONFIG_SP1_BT_DOWNLOAD_ARM) || defined(CONFIG_SP1_PROVISION)
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
        if (wn < 0 || !bt_wire_cmd_cc(wcmd, wn, CYBT_OP_WRITE_RAM, NULL, 0, NULL, 1000)) {
            printk("DL: DS WRITE_RAM failed at +0x%X\n", off);
            return false;
        }
        if ((off % 4096) == 0) {
            printk("DL: wrote 0x%08X (%u/%u)\n", base + off, off, total);
        }
        report(BT_PROV_WRITE, (int)(off * 100 / total));
    }
    printk("DL: DS write complete, verifying by read-back...\n");
    /* Read-back verify: byte-for-byte compare (findings.md's proof method). */
    for (uint32_t off = 0; off < total; off += CYBT_READ_CHUNK) {
        uint8_t chunk = (total - off) < CYBT_READ_CHUNK ? (uint8_t)(total - off) : CYBT_READ_CHUNK;
        uint8_t cmd[16], rd[CYBT_READ_CHUNK];
        uint16_t dlen = 0;
        int n = cybt_cmd_read_ram(cmd, sizeof(cmd), base + off, chunk);
        if (!bt_wire_cmd_cc(cmd, n, CYBT_OP_READ_RAM, rd, sizeof(rd), &dlen, 1000) || dlen != chunk) {
            printk("DL: read-back failed at +0x%X\n", off);
            return false;
        }
        for (uint16_t i = 0; i < chunk; i++) {
            if (rd[i] != cybt_ds_image[off + i]) {
                printk("DL: read-back MISMATCH at 0x%08X — ABORT\n", base + off + i);
                return false;
            }
        }
        report(BT_PROV_VERIFY, (int)(off * 100 / total));
    }
    printk("DL: read-back MATCH\n");
    return true;
}

/* SS must survive an armed write byte-identical in what it declares — re-read
 * and re-check the DS base. Returns false on the (never-yet-seen) violation. */
static bool ss_recheck(void)
{
    uint8_t ss[SS_LEN];
    uint32_t base;

    if (read_ss(ss) && cybt_ss_ds_base(ss, SS_LEN, &base) && base == CYBT_DS_BASE) {
        printk("DL: SS intact after write (DS base still 0x%08X)\n", base);
        return true;
    }
    printk("DL: WARNING — SS re-check failed after write\n");
    return false;
}

/* Warm-boot the module out of download mode into the freshly written app. */
static void warm_boot(void)
{
    uint8_t cmd[16];
    int n = cybt_cmd_launch_ram(cmd, sizeof(cmd), CYBT_LAUNCH_REBOOT);
    bt_wire_tx(cmd, n);
}
#endif /* CONFIG_SP1_BT_DOWNLOAD_ARM || CONFIG_SP1_PROVISION */

#ifdef CONFIG_SP1_BT_DOWNLOAD
void bt_download_run(void)
{
    /* Bring up the USB-CDC console (no SWD/RTT) and give a host a couple of
     * seconds to attach before the log starts. Feed the WDT across the settle. */
    usbdev_start();
    for (int i = 0; i < 25; i++) {
        feed_wdt();
        k_msleep(100);
    }
    bt_wire_init();      /* interrupt-driven RX before any UART traffic */

#ifdef CONFIG_SP1_BT_DOWNLOAD_ARM
    printk("\n=== sp1-beacon MODULE FLASHER — ARMED (will WRITE the DS) ===\n");
#else
    printk("\n=== sp1-beacon MODULE FLASHER — DRY-RUN (no write) ===\n");
#endif
    printk("DS base 0x%08X, DS floor 0x%08X. No chip-erase compiled.\n",
           CYBT_DS_BASE, CYBT_DS_FLOOR);

    if (!bt_wire_enter_download()) {
        bt_wire_halt("DL: halted (download-mode entry failed).");
    }
    /* Identity gate BEFORE the minidriver (findings.md): a mismatch aborts
     * before any write machinery is even loaded. */
    if (!identity_gate() || !load_minidriver() || !ds_plan()) {
        bt_wire_halt("DL: halted (aborted before any write).");
    }

#ifdef CONFIG_SP1_BT_DOWNLOAD_ARM
    printk("DL: ARMED — writing DS now.\n");
    if (!ds_write_and_verify()) {
        bt_wire_halt("DL: halted (write/verify failed — re-enter download and retry).");
    }
    (void)ss_recheck();
    warm_boot();
    printk("DL: DONE — module warm-booted into the new app.\n");
#else
    printk("DL: DRY-RUN complete — identity + plan OK, nothing written.\n");
    printk("DL: build with CONFIG_SP1_BT_DOWNLOAD_ARM=y to perform the write.\n");
#endif

    bt_wire_halt("DL: halted.");
}
#endif /* CONFIG_SP1_BT_DOWNLOAD */

#ifdef CONFIG_SP1_PROVISION
/* On-device provisioning, callable from the normal control loop (see
 * bt_download.h). The same engine as the dev flasher, with the write always
 * compiled, the SS template gate always enforced, and failure RETURNING so
 * the caller keeps its •• power-off escape (every step is timeout-bounded, so
 * this cannot spin forever; a true hang WDT-resets, recoverable). */
bool bt_provision_run(bt_prov_progress_t progress)
{
    bool ok = false;

    prov_cb = progress;
    report(BT_PROV_PREP, 0);
    printk("\n=== sp1-beacon RADIO PROVISIONING (DS-only, SS-preserving) ===\n");
    printk("DL: DS base 0x%08X, floor 0x%08X. No chip-erase compiled.\n",
           CYBT_DS_BASE, CYBT_DS_FLOOR);

    bt_wire_init();                  /* takes over the module UART from module_link */
    if (!bt_wire_enter_download()) {
        printk("DL: provisioning aborted (download-mode entry failed)\n");
        goto out;
    }
    report(BT_PROV_PREP, 10);
    if (!identity_gate()) {          /* incl. the SS template-equality gate */
        printk("DL: provisioning REFUSED (identity/template gate)\n");
        goto out;
    }
    report(BT_PROV_PREP, 20);
    if (!load_minidriver() || !ds_plan()) {
        printk("DL: provisioning aborted before any write\n");
        goto out;
    }
    if (!ds_write_and_verify()) {
        printk("DL: provisioning write/verify FAILED — safe to retry the gesture\n");
        goto out;
    }
    if (!ss_recheck()) {
        goto out;                    /* violated invariant: loud stop, no warm boot */
    }
    warm_boot();
    printk("DL: provisioning DONE — module warm-booted into the beacon app.\n");
    ok = true;
out:
    prov_cb = NULL;
    return ok;
}
#endif /* CONFIG_SP1_PROVISION */
