/*
 * wiced_hci.c — WICED HCI-Control wire codec (see wiced_hci.h).
 * Pure C, host-tested; the UART shell around it lives in module_link.c.
 */
#include "wiced_hci.h"

int whci_build(uint8_t *out, size_t cap, uint16_t opcode,
               const uint8_t *payload, uint16_t len)
{
    size_t total = 5u + (size_t)len;
    if (out == NULL || cap < total) {
        return -1;
    }
    out[0] = WHCI_PKT_WICED;
    out[1] = (uint8_t)(opcode & 0xFF);        /* wire is little-endian */
    out[2] = (uint8_t)(opcode >> 8);
    out[3] = (uint8_t)(len & 0xFF);
    out[4] = (uint8_t)(len >> 8);
    for (uint16_t i = 0; i < len; i++) {
        out[5 + i] = payload[i];
    }
    return (int)total;
}

enum {
    ST_IDLE = 0,     /* hunting for a packet-type byte */
    ST_HDR,          /* collecting the fixed header after the type byte */
    ST_PAYLOAD,
};

void whci_parser_init(struct whci_parser *p)
{
    p->state = ST_IDLE;
    p->kind = 0;
    p->hdr_got = 0;
    p->len = 0;
    p->got = 0;
    p->discarding = 0;
    p->garbage_bytes = 0;
    p->dropped_frames = 0;
}

/* Header length after the packet-type byte: WICED = opcode u16 + len u16;
 * H4 event = event u8 + plen u8. */
static uint8_t hdr_len(uint8_t kind)
{
    return (kind == WHCI_PKT_WICED) ? 4 : 2;
}

static int frame_done(struct whci_parser *p, struct whci_frame *out)
{
    p->state = ST_IDLE;
    if (p->discarding) {
        p->discarding = 0;
        p->dropped_frames++;
        return 0;
    }
    out->kind = p->kind;
    if (p->kind == WHCI_PKT_WICED) {
        out->opcode = (uint16_t)(p->hdr[0] | ((uint16_t)p->hdr[1] << 8));
        out->event = 0;
    } else {
        out->opcode = 0;
        out->event = p->hdr[0];
    }
    out->len = p->len;
    out->payload = p->payload;
    return 1;
}

int whci_parse_byte(struct whci_parser *p, uint8_t byte, struct whci_frame *out)
{
    switch (p->state) {
    case ST_IDLE:
        if (byte == WHCI_PKT_WICED || byte == WHCI_PKT_HCI_EVT) {
            p->kind = byte;
            p->hdr_got = 0;
            p->got = 0;
            p->discarding = 0;
            p->state = ST_HDR;
        } else {
            p->garbage_bytes++;
        }
        return 0;

    case ST_HDR:
        p->hdr[p->hdr_got++] = byte;
        if (p->hdr_got < hdr_len(p->kind)) {
            return 0;
        }
        p->len = (p->kind == WHCI_PKT_WICED)
            ? (uint16_t)(p->hdr[2] | ((uint16_t)p->hdr[3] << 8))
            : p->hdr[1];
        if (p->len == 0) {
            return frame_done(p, out);
        }
        if (p->len > WHCI_MAX_PAYLOAD) {
            p->discarding = 1;   /* swallow the payload, emit nothing */
        }
        p->got = 0;
        p->state = ST_PAYLOAD;
        return 0;

    case ST_PAYLOAD:
        if (!p->discarding) {
            p->payload[p->got] = byte;
        }
        p->got++;
        if (p->got >= p->len) {
            return frame_done(p, out);
        }
        return 0;

    default:
        p->state = ST_IDLE;
        return 0;
    }
}
