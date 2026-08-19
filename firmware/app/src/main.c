/*
 * main.c — SP-1 BLE beacon firmware (M3a: the broadcast state machine).
 *
 * The SP-1 spends its life in a low-power idle: the CYW20706 radio held in
 * reset, LEDs dark, and a slow scan (~every 40 ms) that raises the BTN_COM
 * rail just long enough to sample the two button ladders and the four faders,
 * then drops it again. Any activity — a button held OR a fader moved past the
 * deadband — wakes the radio and broadcasts the full control state
 * (beacon_state.h payload) as a BLE advertisement, refreshed on every change,
 * until things have been quiet for LINGER_MS. Then back to idle.
 *
 * While a button is held it sags the shared BTN_COM rail and corrupts fader
 * reads (feldd's bench finding), so fader values FREEZE at last-good while the
 * rail is loaded; fader moves are picked up whenever no button is down —
 * including in idle, where a fader move alone wakes the radio.
 *
 * Module app note: until the M3b beacon app is flashed, the module still runs
 * feldd's BLE-MIDI app — it ignores SET_STATE, so on today's hardware a wake
 * broadcasts feldd's presence advertisement (via the ADV command) instead of
 * the state payload. The state machine, timing, and power behavior are
 * identical either way, which is what M3a is for.
 *
 * Boot safety (kept from feldd): links above the TE bootloader, charge-standby
 * gate, charger /CE enable, Track1+4 DFU escape (~3 s at the 20 ms scan), ••
 * 5 s hold = power off / •• wake.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_gpio.h>
#include <nrfx.h>

#include "sp1_board.h"
#include "buttons.h"
#include "controls.h"
#include "led.h"
#include "usbdev.h"
#include "module_link.h"
#include "wiced_hci.h"
#include "beacon_state.h"
#include "wdt.h"
#ifdef CONFIG_SP1_BT_DOWNLOAD
#include "bt_download.h"
#endif
#ifdef CONFIG_SP1_BT_DUMP
#include "bt_dump.h"
#endif

/* ---- watchdog (verbatim from feldd: ~8 s hang backstop, fed per tick) ---- */

void feed_wdt(void)
{
    if (NRF_WDT->RUNSTATUS & WDT_RUNSTATUS_RUNSTATUS_Msk) {
        for (int i = 0; i < 8; i++) {
            if (NRF_WDT->RREN & (1u << i)) {
                NRF_WDT->RR[i] = WDT_RR_RR_Reload;
            }
        }
    }
}

static void ensure_wdt_started(void)
{
    if (NRF_WDT->RUNSTATUS & WDT_RUNSTATUS_RUNSTATUS_Msk) {
        return;                                          /* already running */
    }
    NRF_WDT->CONFIG = (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos);
    NRF_WDT->CRV    = 8u * 32768u - 1u;                  /* ~8 s @ 32.768 kHz */
    NRF_WDT->RREN   = WDT_RREN_RR0_Msk;
    NRF_WDT->TASKS_START = 1u;
}

/* ---- BQ24232 charger (feldd/looper-verified: /CE low or the cell never
 * charges and the device browns out at random) ---- */

static void charger_init(void)
{
    nrf_gpio_pin_clear(SP1_CHG_NCE);
    nrf_gpio_cfg_output(SP1_CHG_NCE);
    nrf_gpio_pin_clear(SP1_CHG_NCE);                   /* /CE low = charging */
    nrf_gpio_cfg_input(SP1_CHG_NCHG,   NRF_GPIO_PIN_PULLUP);
    nrf_gpio_cfg_input(SP1_CHG_NPGOOD, NRF_GPIO_PIN_PULLUP);
}

static int usb_present(void) { return nrf_gpio_pin_read(SP1_CHG_NPGOOD) == 0; }
static int charging(void)    { return nrf_gpio_pin_read(SP1_CHG_NCHG)   == 0; }

/* ---- power off: park everything, arm •• sense-low wake, SYSTEM_OFF ---- */

static void power_off(void)
{
    module_link_power(false);           /* module in reset: no advertising while "off" */

    for (int i = 0; i < LED_COUNT; i++) {
        led_idx(i, false);
    }
    controls_rail(0);                   /* stop powering the ladders/faders */

    /* •• is still held (we get here after a long hold). Arming sense-low now
     * would re-wake instantly, so wait for release first, feeding the WDT. */
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

/* ---- battery gauge (calibration from feldd) ---- */

#define BATT_RAW_EMPTY 1962
#define BATT_RAW_FULL  2378
static int battery_pct(int raw)
{
    if (raw < 0) {
        return -1;
    }
    int pct = (raw - BATT_RAW_EMPTY) * 100 / (BATT_RAW_FULL - BATT_RAW_EMPTY);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

static void charge_gauge(int pct, int chg, uint32_t blink)
{
    if (!chg) {                                  /* charge complete: all solid */
        for (int i = 0; i < 4; i++) {
            led_idx(4 + i, true);
        }
        return;
    }
    int n_full = (pct < 0) ? 0 : pct / 25;
    if (n_full > 4) n_full = 4;
    int blink_on = ((blink / 12u) & 1u);
    for (int i = 0; i < 4; i++) {
        led_idx(4 + i, (i < n_full) ? 1 : (i == n_full && n_full < 4) ? blink_on : 0);
    }
}

/* CHARGE-STANDBY GATE (kept from feldd/looper): only a •• wake or a watchdog
 * recovery is a real turn-on; for every other power event, park showing the
 * battery gauge (on USB) or drop to SYSTEM_OFF (on battery) so a full boot can
 * never brown-out-thrash a low cell. Returns only on a real turn-on. */
static void charge_standby_gate(uint32_t wake_reas)
{
    if (wake_reas & (POWER_RESETREAS_OFF_Msk | POWER_RESETREAS_DOG_Msk)) {
        return;
    }

    int64_t hold_t = -1;
    uint32_t tick = 0;
    int adc_up = 0;
    int last_raw = -1;
    for (;;) {
        feed_wdt();
        if (nrf_gpio_pin_read(SP1_FUNC_BTN) == 0) {   /* •• pressed */
            if (hold_t < 0) {
                hold_t = k_uptime_get();
            } else if (k_uptime_get() - hold_t >= 600) {
                break;                                /* held ~0.6 s: power on */
            }
            led_idx(0, true);
        } else {
            hold_t = -1;
            if (!usb_present()) {
                power_off();                          /* battery + idle: off */
            }
            led_idx(0, false);
            if (!adc_up) {
                controls_init();
                led_set_brightness(LED_BRIGHTNESS_FULL);
                last_raw = controls_read_raw(6);      /* battery */
                adc_up = 1;
            } else if ((tick % 50u) == 0u) {
                last_raw = controls_read_raw(6);
            }
            charge_gauge(battery_pct(last_raw), charging(), tick);
        }
        k_msleep(40);
        tick++;
    }

    led_set_brightness(LED_BRIGHTNESS_DEFAULT);
    for (int i = 0; i < 4; i++) {
        led_idx(4 + i, false);
    }
}

/* Boot cue: quick track-LED 1->2->3->4 sweep. */
static void boot_signature(void)
{
    for (int i = 0; i < 4; i++) {
        led_idx(i, true);
        k_msleep(60);
        led_idx(i, false);
    }
}

/* ---- the broadcast state machine ---- */

#define POLL_IDLE_MS    40      /* idle scan cadence (rail duty-cycled) */
#define POLL_ON_MS      20      /* active scan cadence (rail held on) */
#define RAIL_SETTLE_US  500     /* rail-up to first sample. BENCH-TUNE: if idle
                                 * scans misread (phantom wakes / missed
                                 * presses), this is the first knob. */
#define LINGER_MS       5000    /* keep broadcasting this long after the last
                                 * activity before going back to sleep */
#define WAKE_TIMEOUT_MS 3000    /* module boot watchdog: give up and retry */
#define KEEPALIVE_MS    1000    /* re-send the unchanged state this often */
#define BATT_PERIOD_MS  5000    /* battery sample cadence while awake */
#define FUNC_OFF_TICKS  (5000 / POLL_ON_MS)

enum bc_state { BC_IDLE, BC_WAKE, BC_ON };

static const char *const btn_name[BTN_COUNT] = {
    "PLAY", "T1", "T2", "T3", "T4", "VOL+", "VOL-", "FWD", "RWD",
};

static enum bc_state bc = BC_IDLE;
static struct beacon_state st;         /* live control state */
static struct beacon_state last_sent;  /* what's currently on the air */
static uint8_t seq;
static int64_t wake_t, activity_t, last_send_t, last_batt_t;

static void state_send(void)
{
    uint8_t payload[BEACON_STATE_LEN];

    (void)beacon_state_encode(&st, seq, payload, sizeof(payload));
    (void)module_link_send(WHCI_FELDD_SET_STATE, payload, BEACON_STATE_LEN);
    last_sent = st;
    last_send_t = k_uptime_get();
}

/* Scan buttons + faders into st. Fader reads are gated on the rail being
 * unloaded (a held button sags the rail and corrupts them: they freeze at
 * last-good instead). Returns 1 if any button event fired this tick. */
static int scan_controls(void)
{
    struct button_event evt[BTN_COUNT];
    int n = buttons_scan(evt, BTN_COUNT);

    for (int i = 0; i < n; i++) {
        printk("BTN %s %s\n", btn_name[evt[i].idx], evt[i].pressed ? "down" : "up");
        if (evt[i].pressed) {
            st.buttons |= (uint16_t)(1u << evt[i].idx);
        } else {
            st.buttons &= (uint16_t)~(1u << evt[i].idx);
        }
    }

    if (!buttons_rail_probe()) {           /* rail clean: faders are trustworthy */
        for (int i = 0; i < 4; i++) {
            int raw = controls_read_raw(2 + i);
            if (raw >= 0) {
                st.fader[i] = (uint8_t)(raw >> 4);
            }
        }
    }
    return n;
}

static void bc_go_idle(void)
{
    (void)module_link_adv(false);          /* courtesy; the reset kills it anyway */
    module_link_power(false);
    controls_rail(0);
    led_pin(SP1_LED1, false);
    bc = BC_IDLE;
    printk("BC: idle (rail duty-cycled, module in reset)\n");
}

static void bc_wake(const char *why)
{
    printk("BC: wake (%s)\n", why);
    controls_rail(1);                      /* rail stays on while awake */
    module_link_power(true);
    wake_t = k_uptime_get();
    activity_t = wake_t;
    bc = BC_WAKE;
}

int main(void)
{
    uint32_t wake_reas = NRF_POWER->RESETREAS;   /* read BEFORE clearing */
    NRF_POWER->RESETREAS = 0xFFFFFFFFu;
    NRF_POWER->GPREGRET = 0;    /* clear a stale DFU flag; the WDT is the net */

    ensure_wdt_started();
    led_init();
    nrf_gpio_cfg_input(SP1_FUNC_BTN, NRF_GPIO_PIN_PULLUP);

    /* Hold the CYW20706 in reset from the first boot instant, BEFORE the
     * charge-standby gate can SYSTEM_OFF: driven GPIO state is retained through
     * SYSTEM_OFF, so the module can never advertise while "off and charging". */
    nrf_gpio_cfg_output(NRF_GPIO_PIN_MAP(0, 10));
    nrf_gpio_pin_clear(NRF_GPIO_PIN_MAP(0, 10));

    charger_init();

#ifdef CONFIG_SP1_BT_DOWNLOAD
    /* DEV-ONLY module flasher: runs here — before the charge-standby gate — so
     * a plain reset drives it with no •• wake needed on a USB-powered unit.
     * The WDT is already started and USB comes up inside bt_download_run; it
     * never returns, so the gate + control loop below are skipped. */
    bt_download_run();
#endif
#ifdef CONFIG_SP1_BT_DUMP
    /* DEV-ONLY read-only flash dumper — same placement/rationale as the flasher;
     * never returns. */
    bt_dump_run();
#endif

    charge_standby_gate(wake_reas);

    boot_signature();

    controls_init();
    buttons_init();
    module_link_init();         /* UART up; module stays in reset until asked */

    /* USB is the console, but it's also idle-power poison — only bring it up
     * when VBUS is actually present (bench). On battery it stays down; if the
     * cable appears later, the loop below starts it then. */
    int usb_up = 0;
    if (usb_present()) {
        usb_up = (usbdev_start() == 0);
    }

    printk("sp1-beacon M3a (wake=%08x)\n", wake_reas);

    st.battery = BEACON_BATTERY_UNKNOWN;
    last_sent = st;
    uint32_t func_held = 0;
    int      func_armed = 0;    /* honor •• only after it has read released once */
    bc_go_idle();

    for (;;) {
        feed_wdt();

        if (!usb_up && usb_present()) {
            usb_up = (usbdev_start() == 0);
        }

        /* ---- sample the controls (rail handling depends on state) ---- */
        if (bc == BC_IDLE) {
            controls_rail(1);
            k_busy_wait(RAIL_SETTLE_US);
            scan_controls();
            int loaded = buttons_rail_probe();
            controls_rail(0);

            /* Wake on ANY activity: a (even not-yet-debounced) button on the
             * rail, a committed button, or a fader moved past the deadband. */
            if (loaded || st.buttons != 0) {
                bc_wake("button");
            } else if (beacon_state_changed(&last_sent, &st)) {
                bc_wake("fader");
            }
        } else {
            scan_controls();
        }

        /* ---- advance the machine ---- */
        module_link_poll();

        if (bc == BC_WAKE) {
            if (module_link_state() == MODULE_UP) {
                printk("BC: on (module boot %d ms)\n", (int)(k_uptime_get() - wake_t));
                (void)module_link_adv(true);   /* feldd-app compat: presence beacon */
                seq++;
                state_send();
                last_batt_t = 0;               /* force a battery sample soon */
                bc = BC_ON;
            } else if (k_uptime_get() - wake_t > WAKE_TIMEOUT_MS) {
                printk("BC: module boot timeout, retrying via idle\n");
                module_link_power(false);
                bc_go_idle();                  /* still-held buttons re-wake next tick */
            }
        } else if (bc == BC_ON) {
            int64_t now = k_uptime_get();

            if (now - last_batt_t > BATT_PERIOD_MS) {
                last_batt_t = now;
                int pct = battery_pct(controls_read_raw(6));
                if (pct >= 0) {
                    st.battery = (uint8_t)pct;
                }
            }
            if (st.buttons != 0) {
                activity_t = now;              /* holding counts as activity */
            }
            if (beacon_state_changed(&last_sent, &st)) {
                seq++;
                state_send();
                activity_t = now;
            } else if (now - last_send_t > KEEPALIVE_MS) {
                state_send();                  /* same seq: a refresh, not news */
            }
            if (now - activity_t > LINGER_MS) {
                bc_go_idle();
            }
        }

        /* ---- housekeeping ---- */
        /* Track 1+4 held: POWER OFF (not an explicit DFU jump). Powering off is
         * the native primitive — from off, holding Track 1+4 while the
         * bootloader boots (press •• or plug USB) enters DFU the way TE/
         * solderless do it. Every build powers off the same two ways. */
        if (buttons_dfu_held()) {
            printk("Track 1+4 held: powering off (hold 1+4 + USB at boot for DFU)\n");
            power_off();
        }

        /* •• long-hold = power off; short tap logs status (+ pings when up). */
        if (nrf_gpio_pin_read(SP1_FUNC_BTN) == 0) {
            if (func_armed && ++func_held >= FUNC_OFF_TICKS) {
                printk("•• held: powering off\n");
                power_off();
            }
        } else {
            if (func_held > 0 && func_held < FUNC_OFF_TICKS / 4) {
                printk("•• tap: bc=%d module=%d buttons=%03x faders=%u/%u/%u/%u batt=%u seq=%u\n",
                       (int)bc, (int)module_link_state(), st.buttons,
                       st.fader[0], st.fader[1], st.fader[2], st.fader[3],
                       st.battery, seq);
                if (module_link_state() == MODULE_UP) {
                    (void)module_link_ping();
                }
            }
            func_held = 0;
            func_armed = 1;
        }

        /* Side LED 4: on while broadcasting. */
        led_pin(SP1_LED1, bc == BC_ON);

        k_msleep(bc == BC_IDLE ? POLL_IDLE_MS : POLL_ON_MS);
    }
    return 0;
}
