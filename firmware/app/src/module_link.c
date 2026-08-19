/*
 * module_link.c — runtime link to the CYW20706 module (see module_link.h).
 *
 * NORMAL-mode only: boots the module's flashed BLE app and speaks its private
 * WICED-HCI group. The reset/strap sequencing here is the safety-critical bit:
 * P1.01 (module CTS) must be HIGH across a reset release or the mask ROM enters
 * download mode instead of the app (hardware-and-architecture.md §5).
 */
#include "module_link.h"
#include "wiced_hci.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_gpio.h>

#define MODULE_RSTN  NRF_GPIO_PIN_MAP(0, 10)   /* module RST_N, active low */
#define MODULE_CTS   NRF_GPIO_PIN_MAP(1, 1)    /* module CTS <- our GPIO ("RTS") */

/* How long after releasing reset before we drop the module's CTS low (allowing
 * it to transmit). Must be comfortably past the ROM's boot-time strap sample of
 * CTS; 20 ms is a safe ~2x the ~10 ms strap timing in the docs (was 50 —
 * trimmed to shave wake latency; the module's cold boot dominates the rest). */
#define STRAP_SAFE_MS 20

/* If the app hasn't produced a single frame this long after boot, say so on the
 * console (once) — module state stays BOOTING and frames are still logged if
 * they arrive later. */
#define BOOT_QUIET_WARN_MS 3000

static const struct device *uart = DEVICE_DT_GET(DT_NODELABEL(uart0));

static enum module_state state = MODULE_OFF;
static int64_t boot_t;
static bool quiet_warned;
static volatile int last_ack_seq = -1;   /* seq echoed by the latest STATE_ACK */

static struct whci_parser parser;

/* ISR->thread byte ring. 512 covers many frames at 115200 between 8 ms polls
 * (~92 bytes/8 ms line rate). Power-of-two size, head/tail free-running. */
#define RX_RING 512
static uint8_t  ring[RX_RING];
static volatile uint32_t ring_head, ring_tail;
static volatile uint32_t ring_overruns;

static void uart_isr(const struct device *dev, void *user_data)
{
    uint8_t buf[32];
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
        }
        if (n <= 0) {
            break;
        }
    }
}

int module_link_init(void)
{
    /* Module held in reset (BT off) and CTS parked HIGH (never strap) from the
     * first instant, before the UART is even up. */
    nrf_gpio_pin_clear(MODULE_RSTN);
    nrf_gpio_cfg_output(MODULE_RSTN);
    nrf_gpio_pin_set(MODULE_CTS);
    nrf_gpio_cfg_output(MODULE_CTS);

    whci_parser_init(&parser);

    if (!device_is_ready(uart)) {
        printk("BT: uart0 not ready\n");
        return -1;
    }
    uart_irq_callback_user_data_set(uart, uart_isr, NULL);
    uart_irq_rx_enable(uart);
    return 0;
}

void module_link_power(bool on)
{
    if (on) {
        if (state != MODULE_OFF) {
            return;
        }
        /* Normal-boot sequence: CTS HIGH across the reset release (the LOW
         * strap would select download mode), hold reset low a beat, release,
         * then only after the strap window drop CTS so the app may transmit. */
        nrf_gpio_pin_set(MODULE_CTS);
        nrf_gpio_pin_clear(MODULE_RSTN);
        k_msleep(10);
        nrf_gpio_pin_set(MODULE_RSTN);
        k_msleep(STRAP_SAFE_MS);
        nrf_gpio_pin_clear(MODULE_CTS);
        whci_parser_init(&parser);
        state = MODULE_BOOTING;
        boot_t = k_uptime_get();
        quiet_warned = false;
        last_ack_seq = -1;   /* fresh boot: no state acked yet */
        printk("BT: module reset released (normal boot)\n");
    } else {
        if (state == MODULE_OFF) {
            return;
        }
        nrf_gpio_pin_set(MODULE_CTS);       /* park the strap line high */
        nrf_gpio_pin_clear(MODULE_RSTN);    /* hold in reset = BT off */
        state = MODULE_OFF;
        printk("BT: module held in reset\n");
    }
}

enum module_state module_link_state(void)
{
    return state;
}

int module_link_last_ack_seq(void)
{
    return last_ack_seq;
}

static void log_frame(const struct whci_frame *f)
{
    if (f->kind == WHCI_PKT_WICED) {
        printk("BT: rx wiced grp=%02x code=%02x len=%u:",
               WHCI_GROUP(f->opcode), WHCI_CODE(f->opcode), f->len);
    } else {
        printk("BT: rx hci-evt %02x len=%u:", f->event, f->len);
    }
    for (uint16_t i = 0; i < f->len && i < 32; i++) {
        printk(" %02x", f->payload[i]);
    }
    if (f->len > 32) {
        printk(" ...");
    }
    printk("\n");
}

void module_link_poll(void)
{
    struct whci_frame f;

    while (ring_tail != ring_head) {
        uint8_t b = ring[ring_tail % RX_RING];
        ring_tail++;
        if (!whci_parse_byte(&parser, b, &f)) {
            continue;
        }
        log_frame(&f);
        /* Any event in the FELDD private group proves the app is up; STATE_ACK
         * additionally confirms a SET_STATE was applied (advertising started)
         * and echoes its seq, which the broadcast loop uses to stop re-sending. */
        if (f.kind == WHCI_PKT_WICED && WHCI_GROUP(f.opcode) == WHCI_GROUP_FELDD) {
            if (state == MODULE_BOOTING) {
                state = MODULE_UP;
                printk("BT: module UP (first FELDD event %d ms after reset)\n",
                       (int)(k_uptime_get() - boot_t));
            }
            if (WHCI_CODE(f.opcode) == WHCI_FELDD_STATE_ACK && f.len >= 1) {
                last_ack_seq = f.payload[0];
            }
        }
    }

    if (state == MODULE_BOOTING && !quiet_warned &&
        k_uptime_get() - boot_t > BOOT_QUIET_WARN_MS) {
        quiet_warned = true;
        printk("BT: no frame %d ms after boot (garbage=%u overruns=%u)\n",
               BOOT_QUIET_WARN_MS, parser.garbage_bytes, ring_overruns);
    }
}

int module_link_send(uint8_t code, const uint8_t *payload, uint16_t len)
{
    uint8_t buf[64];

    if (state == MODULE_OFF) {
        return -1;
    }
    int n = whci_build(buf, sizeof(buf), WHCI_OPCODE(WHCI_GROUP_FELDD, code),
                       payload, len);
    if (n < 0) {
        return -1;
    }
    printk("BT: tx grp=f0 code=%02x len=%u\n", code, len);
    for (int i = 0; i < n; i++) {
        uart_poll_out(uart, buf[i]);
    }
    return 0;
}

int module_link_adv(bool enable)
{
    uint8_t arg = enable ? 1 : 0;

    return module_link_send(WHCI_FELDD_ADV, &arg, 1);
}
