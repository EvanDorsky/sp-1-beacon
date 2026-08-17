#ifndef BT_DOWNLOAD_H
#define BT_DOWNLOAD_H
/*
 * bt_download.h — dev-only CYW20706 module flasher (M4).
 *
 * Built only under CONFIG_SP1_BT_DOWNLOAD; runs INSTEAD of the normal control
 * loop and never returns. It reflashes the module's Data Section (the beacon
 * app) via the SS-preserving Upgrade Download documented in bluetooth/
 * (reflashing-the-module.md §3, hardware-and-architecture.md §7-8), using the
 * host-tested pure core in cybt_dl.c.
 *
 * Non-destructive by default: every step through the identity gate and a DS
 * dry-run runs UNLESS CONFIG_SP1_BT_DOWNLOAD_ARM is set. There is no
 * chip-erase path anywhere in this subsystem.
 */
void bt_download_run(void);   /* never returns */

#endif /* BT_DOWNLOAD_H */
