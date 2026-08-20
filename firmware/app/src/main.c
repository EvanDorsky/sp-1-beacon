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
                led_set_brightness(LED_BRIGHTNESS_DEFAULT);
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
#define POLL_ON_MS      8       /* active scan cadence (rail held on). The button
                                 * debounce is a 3-read sticky filter designed for
                                 * ~8 ms scans; at the old 20 ms a press took ~40 ms
                                 * of hold to register and short taps were dropped
                                 * before the press latch ever saw them. */
#define RAIL_SETTLE_US  2000     /* rail-up to first sample. BENCH-TUNE: if idle
                                 * scans misread (phantom wakes / missed
                                 * presses), this is the first knob. */
#define LINGER_MS       5000    /* keep broadcasting this long after the last
                                 * activity before going back to sleep */
#define WAKE_TIMEOUT_MS 3000    /* module boot watchdog: give up and retry */
#define KEEPALIVE_MS    1000    /* re-send the unchanged state this often */
#define SET_STATE_RETRY_MS 50   /* before the module acks, re-send this fast so a
                                 * cold-boot-race SET_STATE isn't lost for ~1 s */
#define MIN_PRESS_MS    300     /* hold a press in the broadcast at least this long
                                 * of CONFIRMED on-air time (from the module's ack),
                                 * so even a tap released during the cold boot is
                                 * advertised + caught before its release goes out */
#define MAX_HOLD_MS     700     /* hard cap: never hold a released press longer than
                                 * this from first broadcast, so a module that stops
                                 * acking can't wedge the latch (and idle) forever */
#define GAUGE_BATT_MS   10000   /* charge-gauge battery sample cadence (USB only) */
#define FUNC_OFF_MS     2000    /* •• held this long powers the device off */
#define FUNC_TAP_MS     1000    /* •• released before this = a status tap, not a hold */

enum bc_state { BC_IDLE, BC_WAKE, BC_ON };

static const char *const btn_name[BTN_COUNT] = {
    "PLAY", "T1", "T2", "T3", "T4", "VOL+", "VOL-", "FWD", "RWD",
};

static enum bc_state bc = BC_IDLE;
static struct beacon_state st;         /* live control state */
static struct beacon_state last_sent;  /* what's currently on the air */
static uint8_t seq;
static int64_t wake_t, activity_t, last_send_t;
/* Per-button press latch: a press is recorded the instant it's detected and held
 * in the broadcast state until it has been on the air for MIN_PRESS_MS, so a tap
 * survives the module's cold boot and reaches the receiver. */
static bool    vhold[BTN_COUNT];       /* press latched into the broadcast */
static int64_t vhold_bc[BTN_COUNT];    /* uptime first broadcast, or -1 (max-hold cap ref) */
static int64_t vhold_air[BTN_COUNT];   /* uptime the module acked it on air, or -1 */
static bool    idle_rail_on;           /* idle rail currently powered (USB: kept on) */

static void state_send(const struct beacon_state *s, int64_t now)
{
    uint8_t payload[BEACON_STATE_LEN];

    (void)beacon_state_encode(s, seq, payload, sizeof(payload));
    (void)module_link_send(WHCI_FELDD_SET_STATE, payload, BEACON_STATE_LEN);
    last_sent = *s;
    last_send_t = now;
    /* Note when each still-pending press first went out — the max-hold cap
     * counts from here (the on-air dwell itself counts from the module's ack). */
    for (int i = 0; i < BTN_COUNT; i++) {
        if ((s->buttons & (uint16_t)(1u << i)) && vhold[i] && vhold_bc[i] < 0) {
            vhold_bc[i] = now;
        }
    }
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
            if (!vhold[evt[i].idx]) {          /* new press: latch it the instant it's detected */
                vhold[evt[i].idx] = true;
                vhold_bc[evt[i].idx] = -1;     /* not yet broadcast */
                vhold_air[evt[i].idx] = -1;    /* not yet acked on air */
            }
        } else {
            st.buttons &= (uint16_t)~(1u << evt[i].idx);   /* physical release; the latch dwell releases it on air */
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

/* Build the state to broadcast: the scanned state, but with each latched press
 * held on the air until MIN_PRESS_MS after it first went out (or, until then,
 * unconditionally). Expires latches whose dwell has elapsed. Call once per tick. */
static void tx_state(struct beacon_state *out, int64_t now)
{
    *out = st;
    uint16_t b = st.buttons;
    for (int i = 0; i < BTN_COUNT; i++) {
        if (!vhold[i]) {
            continue;
        }
        bool phys = (st.buttons & (uint16_t)(1u << i)) != 0;
        bool onair_done = vhold_air[i] >= 0 && now - vhold_air[i] >= MIN_PRESS_MS;
        bool cap_done   = vhold_bc[i]  >= 0 && now - vhold_bc[i]  >= MAX_HOLD_MS;
        if (!phys && (onair_done || cap_done)) {
            vhold[i] = false;                  /* released + advertised long enough (or capped) */
        } else {
            b |= (uint16_t)(1u << i);          /* held, still dwelling, or not yet on air */
        }
    }
    out->buttons = b;
}

/* Record a press the instant the idle scan sees it — from the instantaneous
 * ladder decode, not the debounced edge — so a short tap that ends before the
 * debounce commits during the module's cold boot still reaches the receiver.
 * Sets ONLY the vhold latch; st.buttons stays debounce-driven (a physical hold
 * makes it phys=true; a released tap rides the latch dwell out). */
static void instant_latch(int idx)
{
    if (idx >= 0 && !vhold[idx]) {
        vhold[idx] = true;
        vhold_bc[idx] = -1;
        vhold_air[idx] = -1;
    }
}

static void bc_go_idle(void)
{
    (void)module_link_adv(false);          /* courtesy; the reset kills it anyway */
    module_link_power(false);
    controls_rail(0);
    idle_rail_on = false;                  /* rail dropped; next idle scan re-settles */
    led_pin(SP1_LED1, false);
    for (int i = 0; i < BTN_COUNT; i++) {  /* drop any pending press latch */
        vhold[i] = false;
    }
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
    int64_t  func_since = -1;   /* uptime •• was first seen held (armed), or -1 */
    int      func_armed = 0;    /* honor •• only after it has read released once */
    int64_t  gauge_batt_t = -1; /* last charge-gauge battery sample time, or -1 */
    int      gauge_raw = -1;    /* last battery ADC raw for the gauge */
    bc_go_idle();

    for (;;) {
        feed_wdt();

        int usb_now = usb_present();
        if (!usb_up && usb_now) {
            usb_up = (usbdev_start() == 0);
        }

        /* ---- sample the controls (rail handling depends on state) ---- */
        if (bc == BC_IDLE) {
            /* On USB, keep the rail on and scan fast (see the sleep below) so no
             * press falls in a duty-cycle blind gap; on battery, duty-cycle the
             * rail for power. Settle only when the rail was just raised. */
            if (!idle_rail_on) {
                controls_rail(1);
                k_busy_wait(RAIL_SETTLE_US);
                idle_rail_on = true;
            }
            scan_controls();
            int loaded = buttons_rail_probe();
            int trk_now = -1, vol_now = -1;
            if (loaded) {
                buttons_decode_now(&trk_now, &vol_now);   /* capture the button while the rail is on */
            }
            if (!usb_now) {
                controls_rail(0);                          /* battery: drop the rail for power */
                idle_rail_on = false;
            }

            /* Wake on ANY activity: a (even not-yet-debounced) button on the
             * rail, a committed button, or a fader moved past the deadband. */
            if (loaded || st.buttons != 0) {
                instant_latch(trk_now);   /* record the press NOW, before the debounce commits */
                instant_latch(vol_now);
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
                int64_t now = k_uptime_get();
                printk("BC: on (module boot %d ms)\n", (int)(now - wake_t));
                (void)module_link_adv(true);   /* feldd-app compat: presence beacon */
                int pct = battery_pct(controls_read_raw(6));   /* one battery read per wake */
                if (pct >= 0) {
                    st.battery = (uint8_t)pct;
                }
                struct beacon_state txs;
                tx_state(&txs, now);           /* include any press latched during the boot */
                seq++;
                state_send(&txs, now);
                activity_t = now;              /* linger from the first broadcast, not from wake */
                bc = BC_ON;
            } else if (k_uptime_get() - wake_t > WAKE_TIMEOUT_MS) {
                printk("BC: module boot timeout, retrying via idle\n");
                module_link_power(false);
                bc_go_idle();                  /* still-held buttons re-wake next tick */
            }
        } else if (bc == BC_ON) {
            int64_t now = k_uptime_get();

            struct beacon_state txs;
            tx_state(&txs, now);               /* st + latched presses (min on-air hold) */

            bool changed = beacon_state_changed(&last_sent, &txs);
            if (changed) {
                seq++;
                state_send(&txs, now);
            }
            /* Warm-window timer restarts on every new-state broadcast AND while
             * any button is (virtually) held, so we idle LINGER_MS after the
             * LAST activity — not LINGER_MS after the first press of a run. */
            if (changed || txs.buttons != 0) {
                activity_t = now;
            }
            /* Once the module acks the current state, it's confirmed on the air:
             * start each still-held press's on-air dwell from now. */
            bool acked = module_link_last_ack_seq() == (int)seq;
            if (acked) {
                for (int i = 0; i < BTN_COUNT; i++) {
                    if (vhold[i] && vhold_air[i] < 0 &&
                        (last_sent.buttons & (uint16_t)(1u << i))) {
                        vhold_air[i] = now;
                    }
                }
            }
            /* Nothing changed: keep (re)sending the current state — fast until
             * the module acks this seq (the first SET_STATE after a cold boot
             * can beat the module's BLE stack up), then slow keepalive. */
            if (!changed) {
                int64_t due = acked ? KEEPALIVE_MS : SET_STATE_RETRY_MS;
                if (now - last_send_t >= due) {
                    state_send(&txs, now);
                }
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

        /* •• long-hold = power off; short tap logs status (+ pings when up).
         * Time-based so it stays ~5 s regardless of loop cadence (idle 40 ms vs
         * active 8 ms) — the old tick counter drifted with POLL_*_MS. */
        if (nrf_gpio_pin_read(SP1_FUNC_BTN) == 0) {
            if (func_armed) {
                if (func_since < 0) {
                    func_since = k_uptime_get();
                } else if (k_uptime_get() - func_since >= FUNC_OFF_MS) {
                    printk("•• held: powering off\n");
                    power_off();
                }
            }
        } else {
            if (func_since >= 0 && k_uptime_get() - func_since < FUNC_TAP_MS) {
                printk("•• tap: bc=%d module=%d buttons=%03x faders=%u/%u/%u/%u batt=%u seq=%u\n",
                       (int)bc, (int)module_link_state(), st.buttons,
                       st.fader[0], st.fader[1], st.fader[2], st.fader[3],
                       st.battery, seq);
                if (module_link_state() == MODULE_UP) {
                    (void)module_link_ping();
                }
            }
            func_since = -1;
            func_armed = 1;
        }

        /* Charge gauge on the 4 side LEDs whenever USB is plugged in — a
         * glanceable charge state while the device is powered on. Battery moves
         * slowly so it's sampled every GAUGE_BATT_MS; the blink is time-based so
         * its rate stays steady across the 8/40 ms loop cadence. */
        if (usb_present()) {
            int64_t gnow = k_uptime_get();
            if (gauge_batt_t < 0 || gnow - gauge_batt_t >= GAUGE_BATT_MS) {
                gauge_batt_t = gnow;
                gauge_raw = controls_read_raw(6);
            }
            charge_gauge(battery_pct(gauge_raw), charging(), (uint32_t)(gnow / 40));
        } else if (gauge_batt_t >= 0) {
            for (int i = 0; i < 4; i++) {   /* just unplugged: clear the gauge */
                led_idx(4 + i, false);
            }
            gauge_batt_t = -1;
            gauge_raw = -1;
        }

        /* Idle on USB scans as fast as the awake state (rail stays on) so no tap
         * lands in a blind gap; idle on battery duty-cycles at POLL_IDLE_MS. */
        k_msleep(bc != BC_IDLE ? POLL_ON_MS : (usb_now ? POLL_ON_MS : POLL_IDLE_MS));
    }
    return 0;
}
