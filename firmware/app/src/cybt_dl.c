/* cybt_dl.c — pure CYW20706 download-protocol logic (see cybt_dl.h). No I/O. */
#include "cybt_dl.h"
#include <string.h>

/* Build "01 <op_lo> <op_hi> <plen> [params]" into out. */
static int build_cmd(uint8_t *out, size_t cap, uint16_t opcode,
                     const uint8_t *params, uint8_t plen)
{
    size_t total = 4u + (size_t)plen;
    if (out == NULL || cap < total) {
        return -1;
    }
    out[0] = 0x01;
    out[1] = (uint8_t)(opcode & 0xFF);
    out[2] = (uint8_t)(opcode >> 8);
    out[3] = plen;
    for (uint8_t i = 0; i < plen; i++) {
        out[4 + i] = params[i];
    }
    return (int)total;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

int cybt_cmd_hci_reset(uint8_t *out, size_t cap)
{
    return build_cmd(out, cap, CYBT_OP_HCI_RESET, NULL, 0);
}

int cybt_cmd_download_minidriver(uint8_t *out, size_t cap)
{
    return build_cmd(out, cap, CYBT_OP_DL_MINIDRIVER, NULL, 0);
}

int cybt_cmd_write_ram(uint8_t *out, size_t cap, uint32_t addr,
                       const uint8_t *data, uint8_t n)
{
    uint8_t params[4 + 255];
    if ((size_t)n > sizeof(params) - 4) {
        return -1;
    }
    put_le32(params, addr);
    for (uint8_t i = 0; i < n; i++) {
        params[4 + i] = data[i];
    }
    return build_cmd(out, cap, CYBT_OP_WRITE_RAM, params, (uint8_t)(4 + n));
}

int cybt_cmd_read_ram(uint8_t *out, size_t cap, uint32_t addr, uint8_t len)
{
    uint8_t params[5];
    put_le32(params, addr);
    params[4] = len;
    return build_cmd(out, cap, CYBT_OP_READ_RAM, params, 5);
}

int cybt_cmd_launch_ram(uint8_t *out, size_t cap, uint32_t addr)
{
    uint8_t params[4];
    put_le32(params, addr);
    return build_cmd(out, cap, CYBT_OP_LAUNCH_RAM, params, 4);
}

bool cybt_cc_ok(const uint8_t *evt_payload, uint16_t len, uint16_t opcode,
                const uint8_t **data, uint16_t *dlen)
{
    /* payload: 01 <op_lo> <op_hi> <status> [data...] */
    if (evt_payload == NULL || len < 4) {
        return false;
    }
    uint16_t got_op = (uint16_t)(evt_payload[1] | ((uint16_t)evt_payload[2] << 8));
    if (evt_payload[0] != 0x01 || got_op != opcode || evt_payload[3] != 0x00) {
        return false;
    }
    if (data != NULL) {
        *data = evt_payload + 4;
    }
    if (dlen != NULL) {
        *dlen = (uint16_t)(len - 4);
    }
    return true;
}

bool cybt_addr_in_ds_window(uint32_t addr, uint32_t len)
{
    if (len == 0) {
        return false;
    }
    if (addr < CYBT_DS_FLOOR) {
        return false;               /* would reach VS/SS */
    }
    /* Guard the +len against wrap and the flash ceiling. */
    if (addr > CYBT_FLASH_END || len > CYBT_FLASH_END - addr) {
        return false;
    }
    return true;
}

bool cybt_ss_ds_base(const uint8_t *ss, uint32_t ss_len, uint32_t *out_base)
{
    uint32_t off = 0;

    if (ss == NULL || out_base == NULL) {
        return false;
    }
    /* Records: [type u8][len u16 LE][payload len]. Walk until a type-0x02
     * record, reading its payload[0..3] as the DS base. */
    while (off + 3u <= ss_len) {
        uint8_t  type = ss[off];
        uint16_t rlen = (uint16_t)(ss[off + 1] | ((uint16_t)ss[off + 2] << 8));

        /* An all-0xFF byte where a type is expected = padding after the last
         * real record; stop (no type-0x02 found). */
        if (type == 0xFF) {
            return false;
        }
        if (off + 3u + rlen > ss_len) {
            return false;           /* record runs past the buffer: malformed */
        }
        if (type == 0x02) {
            if (rlen < 4) {
                return false;       /* type-0x02 must carry a 4-byte DS base */
            }
            const uint8_t *p = ss + off + 3u;
            uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            /* The record stores a flash OFFSET (0..flash size, e.g. 0x3000 on
             * the SP-1), captured on hardware. Normalize to the memory-mapped
             * address the write path uses (0xFF000000 + offset). A value that
             * is already mapped (top byte 0xFF) passes through unchanged, so
             * either encoding yields the mapped DS base. */
            if (v < CYBT_FLASH_BASE) {
                v += CYBT_FLASH_BASE;
            }
            *out_base = v;
            return true;
        }
        off += 3u + rlen;
    }
    return false;
}

bool cybt_ss_gate_ok(const uint8_t *ss, uint32_t ss_len)
{
    uint32_t base = 0;
    if (!cybt_ss_ds_base(ss, ss_len, &base)) {
        return false;
    }
    return base == CYBT_DS_BASE;
}

bool cybt_ss_template_ok(const uint8_t *ss, uint32_t ss_len,
                         const uint8_t *tmpl, uint32_t tmpl_len)
{
    if (ss == NULL || tmpl == NULL || tmpl_len == 0 || ss_len < tmpl_len) {
        return false;
    }
    for (uint32_t i = 0; i < tmpl_len; i++) {
        if (i >= CYBT_SS_BDADDR_OFF &&
            i < CYBT_SS_BDADDR_OFF + CYBT_SS_BDADDR_LEN) {
            continue;               /* the per-unit BD_ADDR bytes */
        }
        if (ss[i] != tmpl[i]) {
            return false;
        }
    }
    return true;
}
