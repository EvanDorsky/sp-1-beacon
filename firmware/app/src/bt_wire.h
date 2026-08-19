#ifndef BT_WIRE_H
#define BT_WIRE_H
/*
 * bt_wire.h — shared CYW20706 download-mode wire layer (dev-only).
 *
 * The low-level UART I/O + download-mode entry + power-off escape, factored out
 * of bt_download.c so the flasher (bt_download.c) and the read-only dumper
 * (bt_dump.c) drive the module through the SAME proven code — same strap
 * timing, same autobaud loop, same recovery — rather than a fork.
 *
 * Everything here is NORMAL to the read/verify path; the only thing that writes
 * flash lives in bt_download.c behind CONFIG_SP1_BT_DOWNLOAD_ARM.
 */
#include <stdint.h>
#include <stdbool.h>
#include "wiced_hci.h"   /* struct whci_frame */

/* Bring up interrupt-driven RX on uart0 (returns after enabling the ISR).
 * Safe to call once before any module traffic. */
void bt_wire_init(void);

/* Recovery Reset (CTS-low strap) + autobaud HCI_RESET loop. True once the ROM
 * acks — the module is in download mode. */
bool bt_wire_enter_download(void);

/* Drain the RX ring + reset the frame parser (clean slate before an exchange). */
void bt_wire_flush(void);

/* Raw TX to the module UART. */
void bt_wire_tx(const uint8_t *b, int n);

/* Send an H4 command, wait for its command-complete ack; on a status-0 ack copy
 * up to cap bytes of ack data into data (and set *dlen) and return true. On
 * failure logs what it saw. */
bool bt_wire_cmd_cc(const uint8_t *cmd, int clen, uint16_t opcode,
                    uint8_t *data, uint16_t cap, uint16_t *dlen, int timeout_ms);

/* Power the device OFF (SYSTEM_OFF, arms the •• sense-low wake). Never returns. */
void bt_wire_power_off(void);

/* Print msg, then sit in the recovery loop forever: •• (5 s) or Track 1+4
 * (~1.2 s) held powers off, so a halted dev build is always recoverable.
 * Never returns. */
void bt_wire_halt(const char *msg);

#endif /* BT_WIRE_H */
