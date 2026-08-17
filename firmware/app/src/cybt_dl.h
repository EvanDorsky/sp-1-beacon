#ifndef CYBT_DL_H
#define CYBT_DL_H
/*
 * cybt_dl.h — pure CYW20706 download-protocol logic (no I/O, host-tested).
 *
 * This is the safety-critical core of the module flasher (M4). It builds the
 * download-ROM command bytes, matches their command-complete acks, walks the
 * Static Section to find the DS base, and gates every write address against a
 * compile-time floor. It performs NO I/O — bt_download.c drives it over the
 * UART. Keeping it pure lets firmware/test/test_cybt_dl.c exercise the exact
 * bytes and the exact floor/identity guards on the host.
 *
 * Protocol per bluetooth/hardware-and-architecture.md §7 and
 * reflashing-the-module.md §3. These are raw HCI vendor-specific commands
 * (H4 packet type 0x01), little-endian opcodes on the wire; acks are H4
 * command-complete events (0x04 0x0E ...).
 *
 * SAFETY (the whole point of this module):
 *   - There is NO chip-erase builder here. CHIP_ERASE / Full Download is not
 *     implemented; #define CYBT_DL_ALLOW_CHIP_ERASE is rejected at compile
 *     time so the destructive opcode can never enter the image.
 *   - Every DS write address is checked against CYBT_DS_FLOOR by
 *     cybt_addr_in_ds_window(); a write below the floor (toward the VS/SS) or
 *     past the end of flash is refused. This is a plain runtime check, not an
 *     assertion, so it holds in a release build too.
 *   - The DS base is DISCOVERED (from the SS) only to be COMPARED against the
 *     compile-time CYBT_DS_BASE; discovery can refuse a write, never redirect
 *     one. A mis-parsed SS degrades to a safe abort.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef CYBT_DL_ALLOW_CHIP_ERASE
#error "sp1-beacon never issues CHIP_ERASE / Full Download — see bluetooth/README.md safety headline"
#endif

/* Off-chip serial flash memory map (bluetooth/hardware-and-architecture.md §3,
 * findings.md). SS at base; VS ends at 0xFF002000; the SP-1 module's SS points
 * its DS base at 0xFF003000 (NOT the eval board's 0xFF004000). */
#define CYBT_FLASH_BASE   0xFF000000u
#define CYBT_FLASH_END    0xFF080000u   /* 512 KB */
#define CYBT_VS_END       0xFF002000u
#define CYBT_DS_BASE      0xFF003000u   /* SP-1; verified against the live SS before any write */
#define CYBT_DS_FLOOR     CYBT_DS_BASE  /* writes strictly at/above this, never toward VS/SS */

/* RAM staging address for the minidriver (doc §7; the hex's start-linear-
 * address record should match — gen_blobs.py asserts it). */
#define CYBT_MINIDRIVER_RAM 0x00220000u

/* WRITE_RAM data payload per chunk. Doc: up to 249; reflashing §3 uses 240
 * (0xF0) and steps the address by 0xF0. */
#define CYBT_WRITE_CHUNK  240
/* READ_RAM max data per chunk (len is one byte; keep clear of framing). */
#define CYBT_READ_CHUNK   240

/* HCI vendor-specific opcodes (little-endian on the wire). */
#define CYBT_OP_HCI_RESET      0x0C03
#define CYBT_OP_UPDATE_BAUD    0xFC18
#define CYBT_OP_DL_MINIDRIVER  0xFC2E
#define CYBT_OP_WRITE_RAM      0xFC4C
#define CYBT_OP_READ_RAM       0xFC4D
#define CYBT_OP_LAUNCH_RAM     0xFC4E
#define CYBT_OP_VERIFY_CRC     0xFCCC
/* 0xFFCE CHIP_ERASE is deliberately NOT defined. */

/* LAUNCH_RAM completion-reboot address: warm-boot into the freshly written app
 * (doc §7 — 0x0 core-dumps; 0xFFFFFFFF warm-boots). */
#define CYBT_LAUNCH_REBOOT 0xFFFFFFFFu

/* ---- command builders: write the H4 command bytes into out, return length
 * (>=0), or -1 if out is too small. ---- */
int cybt_cmd_hci_reset(uint8_t *out, size_t cap);
int cybt_cmd_download_minidriver(uint8_t *out, size_t cap);
int cybt_cmd_write_ram(uint8_t *out, size_t cap, uint32_t addr,
                       const uint8_t *data, uint8_t n);
int cybt_cmd_read_ram(uint8_t *out, size_t cap, uint32_t addr, uint8_t len);
int cybt_cmd_launch_ram(uint8_t *out, size_t cap, uint32_t addr);

/* ---- ack matching ----
 * A command-complete event payload (as deframed by whci_parser: the bytes
 * AFTER the 04 0E len header) looks like: 01 op_lo op_hi status [data...].
 * Returns true if it is a command-complete for `opcode` with status 0x00; on
 * success sets *data/*dlen to the trailing data (may be empty). */
bool cybt_cc_ok(const uint8_t *evt_payload, uint16_t len, uint16_t opcode,
                const uint8_t **data, uint16_t *dlen);

/* ---- safety guards ---- */
/* True iff [addr, addr+len) lies entirely within the DS window
 * [CYBT_DS_FLOOR, CYBT_FLASH_END). Any write outside is refused. */
bool cybt_addr_in_ds_window(uint32_t addr, uint32_t len);

/* Walk the SS TLV stream ([type u8][len u16 LE][payload]) and return the DS
 * base from the type-0x02 record's payload[0..3] (LE32). Returns false if the
 * stream is malformed or has no type-0x02 record. Pure; safe on any bytes. */
bool cybt_ss_ds_base(const uint8_t *ss, uint32_t ss_len, uint32_t *out_base);

/* The identity/template gate: the live SS is acceptable ONLY if its discovered
 * DS base equals CYBT_DS_BASE (compile-time). This is compare-only — it can
 * refuse, never redirect. Returns true = safe to proceed. */
bool cybt_ss_gate_ok(const uint8_t *ss, uint32_t ss_len);

#endif /* CYBT_DL_H */
