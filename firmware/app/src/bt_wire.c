/*
 * bt_wire.c — shared CYW20706 download-mode wire layer (see bt_wire.h).
 *
 * Extracted verbatim from bt_download.c so the flasher and the read-only dumper
 * share one proven implementation of the strap timing, autobaud loop, UART ring,
 * and power-off recovery. Log prefix stays "DL:" for continuity with the bench
 * output we've been reading.
 */
#include "bt_wire.h"
#include "cybt_dl.h"
#include "wdt.h"
#include "controls.h"
#include "buttons.h"
#include "led.h"
#include "sp1_board.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_gpio.h>
#include <nrfx.h>

#define MODULE_RSTN  NRF_GPIO_PIN_MAP(0, 10)   /* RST_N, active low */
#define MODULE_CTS   NRF_GPIO_PIN_MAP(1, 1)    /* module CTS <- our GPIO; LOW = download strap */

static const struct device *uart = DEVICE_DT_GET(DT_NODELABEL(uart0));

static struct whci_parser parser;

/* ---- interrupt-driven RX ring (poll-mode UARTE drops bytes on the long
 * READ_RAM responses; buffer in an ISR). ---- */
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

void bt_wire_init(void)
{
    ring_head = ring_tail = ring_overruns = rx_total = 0;
    if (!device_is_ready(uart)) {
        printk("DL: uart0 NOT READY\n");
        return;
    }
    uart_irq_callback_user_data_set(uart, dl_uart_isr, NULL);
    uart_irq_rx_enable(uart);
}

void bt_wire_flush(void)
{
    ring_tail = ring_head;
    whci_parser_init(&parser);
}

void bt_wire_tx(const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) {
        uart_poll_out(uart, b[i]);
    }
}

/* Pull the next complete frame from the ring through the codec, or false on
 * timeout (ms). Feeds the WDT — the sequence spans many seconds. */
static bool rx_frame(struct whci_frame *out, int timeout_ms)
{
    int64_t deadline = k_uptime_get() + timeout_ms;
    while (k_uptime_get() < deadline) {
        feed_wdt();
        while (ring_tail != ring_head) {
            uint8_t byte = ring[ring_tail % RX_RING];
            ring_tail++;
            if (whci_parse_byte(&parser, byte, out)) {
                return true;
            }
        }
        /* SLEEP, don't busy-wait: yields the CPU so lower-priority threads run
         * — critically the USB-CDC TX workqueue that flushes the console. A
         * busy-wait here starved it and the dump stalled once the CDC TX ring
         * filled (~512 B). The RX ISR keeps filling the ring during the sleep. */
        k_msleep(1);
    }
    return false;
}

bool bt_wire_cmd_cc(const uint8_t *cmd, int clen, uint16_t opcode,
                    uint8_t *data, uint16_t cap, uint16_t *dlen, int timeout_ms)
{
    struct whci_frame f;
    uint32_t rx_before = rx_total;
    int seen = 0;

    bt_wire_tx(cmd, clen);
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
        printk("DL:   rx CC op=%02x%02x status=%02x (wanted op=%04x) len=%u\n",
               f.payload[2], f.payload[1], f.len >= 4 ? f.payload[3] : 0xFF,
               opcode, f.len);
    }
    printk("DL:   (no matching ack: %d frame(s) seen, %u rx bytes, %u overruns)\n",
           seen, rx_total - rx_before, ring_overruns);
    return false;
}

bool bt_wire_enter_download(void)
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

    bt_wire_flush();                    /* drop any pre-download boot chatter */
    /* The ROM re-autobauds on entry: the first one or two HCI_RESETs get
     * dropped. Loop until the ack lands (doc §3/§5). */
    for (int attempt = 0; attempt < 20; attempt++) {
        if (bt_wire_cmd_cc(cmd, n, CYBT_OP_HCI_RESET, NULL, 0, NULL, 200)) {
            printk("DL: download mode entered (HCI_RESET ack on attempt %d)\n", attempt + 1);
            return true;
        }
    }
    printk("DL: FAILED to enter download mode (no HCI_RESET ack)\n");
    return false;
}

void bt_wire_power_off(void)
{
    nrf_gpio_pin_clear(MODULE_RSTN);        /* hold the module in reset (BT off) */
    nrf_gpio_cfg_output(MODULE_RSTN);
    nrf_gpio_pin_clear(MODULE_RSTN);
    for (int i = 0; i < LED_COUNT; i++) {
        led_idx(i, false);                  /* LEDs dark = the "off" cue */
    }
    controls_rail(0);                       /* stop powering the ladders */

    /* If •• triggered this it is still held; arming sense-low now would re-wake
     * instantly, so wait for release first (Track1+4 trigger: •• is high, this
     * falls straight through), feeding the WDT meanwhile. */
    while (nrf_gpio_pin_read(SP1_FUNC_BTN) == 0) { feed_wdt(); k_msleep(20); }
    k_msleep(60);

    nrf_gpio_cfg_sense_input(SP1_FUNC_BTN, NRF_GPIO_PIN_PULLUP,
                             NRF_GPIO_PIN_SENSE_LOW);
    NRF_POWER->RESETREAS = 0xFFFFFFFFu;
    __DSB();
    NRF_POWER->SYSTEMOFF = 1u;
    __DSB();
    for (;;) { }
}

void bt_wire_halt(const char *msg)
{
    printk("%s\n", msg);
    printk("DL: hold •• (2 s) or Track 1+4 to power off "
           "(hold 1+4 + plug USB at boot for DFU).\n");
    /* ESCAPE HATCH: dev builds run no control loop, so poll •• (direct GPIO) +
     * the Track 1+4 combo (TRACKS ladder) and power off on a hold. */
    controls_init();
    nrf_gpio_cfg_input(SP1_FUNC_BTN, NRF_GPIO_PIN_PULLUP);
    int func_cnt = 0, trk_cnt = 0;
    for (;;) {
        feed_wdt();
        int func_held = (nrf_gpio_pin_read(SP1_FUNC_BTN) == 0);
        int trk_held  = buttons_in_dfu_band_pure(controls_read_raw(0));
        led_pin(SP1_TRACK_LED1, func_held || trk_held);   /* held-gesture feedback */
        if (func_held) { if (++func_cnt >= 20) bt_wire_power_off(); } else { func_cnt = 0; }  /* ~2 s, matches runtime FUNC_OFF_MS */
        if (trk_held)  { if (++trk_cnt  >= 12) bt_wire_power_off(); } else { trk_cnt  = 0; }  /* ~1.2 s */
        k_msleep(100);
    }
}
