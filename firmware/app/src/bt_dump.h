#ifndef BT_DUMP_H
#define BT_DUMP_H
/*
 * bt_dump.h — dev-only, READ-ONLY dump of the CYW20706 module's serial flash.
 *
 * Built under CONFIG_SP1_BT_DUMP; runs instead of the control loop and never
 * returns. Enters download mode and READ_RAMs the whole 512 KB flash
 * (0xFF000000..0xFF080000) at the ROM level (no minidriver, no write path
 * compiled in at all), streaming the raw bytes over the USB-CDC console framed
 * by DUMPSTART/DUMPEND markers, with an nRF-computed CRC-32 the host re-checks.
 * The result is a full backup — including a restorable copy of the module's
 * current DS (the app), taken before any armed write. See scripts/dump_recv.py.
 */
void bt_dump_run(void);   /* never returns */

#endif /* BT_DUMP_H */
