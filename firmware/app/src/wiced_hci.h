#ifndef WICED_HCI_H
#define WICED_HCI_H
/*
 * wiced_hci.h — minimal WICED HCI-Control wire codec for the nRF <-> CYW20706
 * module UART (AIROC HCI Control Protocol, Infineon doc 002-16618).
 *
 * Two packet kinds share the wire in NORMAL (app-running) mode:
 *   - WICED control frames:  0x19 | opcode u16 LE | length u16 LE | payload
 *     (opcode = group byte << 8 | code byte; captured on hardware, e.g. the
 *     LE-group scan command goes 19 01 01 01 00 01 — see
 *     bluetooth/hardware-and-architecture.md §4/§5)
 *   - Plain H4 HCI events:   0x04 | event u8 | plen u8 | payload
 *     (the download ROM's command-complete acks use this shape; harmless to
 *     understand them in normal mode too)
 *
 * Pure C, no Zephyr deps — host-tested in firmware/test/test_wiced_hci.c.
 */
#include <stdint.h>
#include <stddef.h>

#define WHCI_PKT_WICED   0x19
#define WHCI_PKT_HCI_EVT 0x04

/* The feldd module app's private command group (bluetooth/module-app/README.md
 * "feldd's private WICED-HCI command group"). Command codes are documented
 * there; the module also emits events back in this group (READY, LINK_STATE)
 * whose codes are NOT documented in this tree — treat any group-0xF0 event as
 * proof of app liveness and log it for the bench. */
#define WHCI_GROUP_FELDD      0xF0
#define WHCI_FELDD_MIDI       0x01
#define WHCI_FELDD_HID        0x02
#define WHCI_FELDD_PING       0x03
#define WHCI_FELDD_ADV        0x04
#define WHCI_FELDD_CLEAR_BONDS 0x05
/* beacon-sp-1 extension (the M3b CYW20706 beacon app): payload is the
 * BEACON_STATE_LEN state-beacon bytes (beacon_state.h); the app embeds them
 * in a non-connectable advertisement and (re)starts advertising. feldd's
 * shipped module app ignores this code, so sending it is always harmless. */
#define WHCI_FELDD_SET_STATE  0x10
/* Events the M3b beacon app sends back in the FELDD group: READY (app up / ping
 * reply) and STATE_ACK, whose payload[0] echoes the seq of the SET_STATE it just
 * applied. The host watches for STATE_ACK to know a broadcast actually started —
 * the first SET_STATE after a module cold boot can arrive before its BLE stack
 * is up, so nothing advertises and no ack comes back until the host re-sends. */
#define WHCI_FELDD_READY      0x80
#define WHCI_FELDD_STATE_ACK  0x81

#define WHCI_OPCODE(group, code) ((uint16_t)(((uint16_t)(group) << 8) | (code)))
#define WHCI_GROUP(opcode)       ((uint8_t)((opcode) >> 8))
#define WHCI_CODE(opcode)        ((uint8_t)((opcode) & 0xFF))

/* Largest payload we accept from the module. WICED frames carry a u16 length;
 * anything bigger than this is discarded (counted in dropped_frames) so a
 * corrupt length byte can't wedge the parser. */
#define WHCI_MAX_PAYLOAD 264

/* Build a WICED control frame into out. Returns total frame length (5 + len),
 * or -1 if it doesn't fit in cap. payload may be NULL when len is 0. */
int whci_build(uint8_t *out, size_t cap, uint16_t opcode,
               const uint8_t *payload, uint16_t len);

struct whci_frame {
    uint8_t  kind;      /* WHCI_PKT_WICED or WHCI_PKT_HCI_EVT */
    uint16_t opcode;    /* WICED frames: group<<8 | code */
    uint8_t  event;     /* H4 events: event code */
    uint16_t len;
    const uint8_t *payload;   /* borrows the parser's buffer — consume before
                               * the next whci_parse_byte() call */
};

struct whci_parser {
    uint8_t  state;
    uint8_t  kind;
    uint8_t  hdr[4];
    uint8_t  hdr_got;
    uint16_t len;
    uint16_t got;
    uint8_t  payload[WHCI_MAX_PAYLOAD];
    uint8_t  discarding;        /* oversize frame: swallow payload, don't emit */
    uint32_t garbage_bytes;     /* bytes skipped hunting for a packet-type byte */
    uint32_t dropped_frames;    /* oversize frames discarded whole */
};

void whci_parser_init(struct whci_parser *p);

/* Push one received byte. Returns 1 when a complete frame is in *out
 * (valid until the next call), else 0. Unknown packet-type bytes are skipped
 * and counted so the stream re-syncs after line noise. */
int whci_parse_byte(struct whci_parser *p, uint8_t byte, struct whci_frame *out);

#endif /* WICED_HCI_H */
