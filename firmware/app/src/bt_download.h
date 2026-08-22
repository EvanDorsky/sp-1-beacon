#ifndef BT_DOWNLOAD_H
#define BT_DOWNLOAD_H
/*
 * bt_download.h — CYW20706 module flasher (M4) + on-device provisioning.
 *
 * Two entry points into ONE flashing engine (SS-preserving DS-only Upgrade
 * Download, bluetooth/reflashing-the-module.md §3, host-tested pure core in
 * cybt_dl.c; NO chip-erase path exists anywhere in this subsystem):
 *
 *  - bt_download_run() — DEV-ONLY (CONFIG_SP1_BT_DOWNLOAD): runs INSTEAD of
 *    the normal control loop at boot and never returns. Non-destructive dry
 *    run unless CONFIG_SP1_BT_DOWNLOAD_ARM is also set.
 *
 *  - bt_provision_run() — RELEASE (CONFIG_SP1_PROVISION): callable FROM the
 *    normal control loop (the Play-hold consent gesture) to flash the beacon
 *    app onto a radio in any state (stock TE / feldd / older ours). Gated by
 *    the full SS template-equality check (factory template, BD_ADDR-masked) —
 *    an unknown SS refuses, never writes. Returns true on verified success
 *    (the module is warm-booted into the new app), false on refusal/failure
 *    (the module may be left in download mode; the next normal
 *    module_link_power(true) reset recovers it). The caller must re-own the
 *    module UART afterwards (module_link_init()).
 */
#include <stdbool.h>

void bt_download_run(void);   /* dev-only boot mode; never returns */

/* Provisioning progress, for the caller's LED display. phase advances
 * PREP -> WRITE -> VERIFY; pct is 0..100 within the phase. */
enum bt_prov_phase { BT_PROV_PREP, BT_PROV_WRITE, BT_PROV_VERIFY };
typedef void (*bt_prov_progress_t)(enum bt_prov_phase phase, int pct);

bool bt_provision_run(bt_prov_progress_t progress);

#endif /* BT_DOWNLOAD_H */
