/*
 * main.c — SP-1 BTHome beacon firmware (the broadcast state machine).
 *
 * The SP-1 spends its life in a low-power idle: the CYW20706 radio held in
 * reset, LEDs dark, and a slow scan (~every 40 ms on battery; continuous on
 * USB) of the button ladders. A button press wakes the radio; the debounced
 * press is classified into a BTHome v2 EVENT (press instantly at the down
 * edge; long_press additionally at the threshold), queued, and broadcast as
 * data (bthome.h, bluetooth/broadcast-format.md) that Home Assistant decodes
 * natively. Each event gets its own packet id and is held on the air for an
 * acked dwell so a duty-cycled receiver catches it; keepalives repeat the
 * same pid (receivers dedup on it). LINGER_MS after the last activity the
 * radio powers back down.
 *
 * While a button is held it sags the shared BTN_COM rail and corrupts fader
 * reads (bench finding), so fader values FREEZE at last-good while the rail
 * is loaded. Faders are read but not yet broadcast (phase 2) and no longer
 * wake the radio.
 *
 * Boot safety (kept from feldd): links above the TE bootloader, charge-standby
 * gate, charger /CE enable, Track1+4 power-off, •• hold = power off / •• wake.
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
#include "bthome.h"
#include "wdt.h"
#if defined(CONFIG_SP1_BT_DOWNLOAD) || defined(CONFIG_SP1_PROVISION)
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
#define WAKE_TIMEOUT_MS 5000    /* module boot watchdog: give up and retry */
#define KEEPALIVE_MS    1000    /* re-send the unchanged packet this often */
#define SEND_RETRY_MS   50      /* before the module acks, re-send this fast so a
                                 * cold-boot-race SET_STATE isn't lost for ~1 s */
#define EVENT_DWELL_MS  600     /* keep an event packet on the air at least this
                                 * long of CONFIRMED (acked) time before the next
                                 * queued event may replace it — rides out the
                                 * receiver's WiFi-coex scan gaps (bench-tested) */
#define EVENT_CAP_MS    1000    /* hard cap from first send: a module that stops
                                 * acking can't wedge the event queue forever */
#define FADER_RAW_FULL  3650    /* raw ADC at full fader deflection. The 3.6 V
                                 * ADC full-scale (gain 1/6, 0.6 V internal ref)
                                 * exceeds the ~3.3 V rail, so raw tops out
                                 * ~3700 (231 after >>4); rescale so the top of
                                 * travel reliably reads 255. Bench: •• tap
                                 * showed 231 max, 2026-08-21. */
#define FADER_MIN_MS    50      /* min gap between fader packets: sliding streams
                                 * ~5 updates/s; button events always take priority */
#define GAUGE_BATT_MS   10000   /* charge-gauge battery sample cadence (USB only) */
#define FUNC_OFF_MS     5000    /* •• held this long powers the device off */
#define FUNC_TAP_MS     1000    /* •• released before this = a status tap, not a hold */

enum bc_state { BC_IDLE, BC_WAKE, BC_ON };

static const char *const btn_name[BTN_COUNT] = {
    "PLAY", "T1", "T2", "T3", "T4", "VOL+", "VOL-", "FWD", "RWD",
};

static enum bc_state bc = BC_IDLE;
static struct bthome_clf clf;          /* debounced edges -> press/long_press events */
static uint8_t fader[4];               /* live fader values (rail-frozen while a button sags it) */
static uint8_t last_bcast_fader[4];    /* what's been broadcast; dirty = moved past the deadband */
static bool    fader_seeded;           /* first clean read seeds the baseline, no phantom wake */
static uint8_t battery = BTHOME_BATT_UNKNOWN;
static uint8_t pid;                    /* BTHome packet id: bumps per EVENT, survives sleeps */
static int64_t wake_t, activity_t, last_send_t;
static bool    idle_rail_on;           /* idle rail currently powered (USB: kept on) */

/* Pending button events (FIFO). Each event gets its own packet id and holds the
 * air for EVENT_DWELL_MS of acked time before the next one may replace it. */
#define EVQ_CAP 8
static struct { uint8_t btn, ev; } evq[EVQ_CAP];
static int evq_head, evq_len;

static void evq_push(uint8_t btn, uint8_t ev)
{
    if (evq_len >= EVQ_CAP) {
        printk("BC: event queue full, dropping %s\n", btn_name[btn]);
        return;
    }
    int slot = (evq_head + evq_len) % EVQ_CAP;
    evq[slot].btn = btn;
    evq[slot].ev = ev;
    evq_len++;
}

static bool evq_pop(uint8_t *btn, uint8_t *ev)
{
    if (evq_len == 0) {
        return false;
    }
    *btn = evq[evq_head].btn;
    *ev = evq[evq_head].ev;
    evq_head = (evq_head + 1) % EVQ_CAP;
    evq_len--;
    return true;
}

/* The current on-air packet: kind + content for the current pid — content is
 * NEVER changed without bumping pid, so receivers that dedup on pid can't see
 * two contents under one id. */
enum pkt_kind { PKT_BUTTONS, PKT_FADERS };
static enum pkt_kind cur_kind = PKT_BUTTONS;
static uint8_t cur_ev[BTHOME_BTN_COUNT];
static uint8_t cur_fader[4];           /* snapshot broadcast in a PKT_FADERS packet */
static bool    cur_has_event;
static int64_t pkt_send_t;             /* first send of the current pid */
static int64_t pkt_air_t;              /* first ack of the current pid, -1 = not yet */

static void payload_send(int64_t now)
{
    uint8_t p[BTHOME_MAX_PAYLOAD];
    int n = (cur_kind == PKT_FADERS)
                ? bthome_encode_faders(pid, battery, cur_fader, p, sizeof(p))
                : bthome_encode(pid, battery, cur_ev, p, sizeof(p));

    if (n > 0) {
        (void)module_link_send(WHCI_FELDD_SET_STATE, p, (uint16_t)n);
    }
    last_send_t = now;
}

/* Start a new button packet: bump pid, set the content (btn < 0 = all-none), send. */
static void packet_new(int btn, uint8_t ev, int64_t now)
{
    cur_kind = PKT_BUTTONS;
    for (int i = 0; i < BTHOME_BTN_COUNT; i++) {
        cur_ev[i] = BTHOME_EV_NONE;
    }
    cur_has_event = false;
    if (btn >= 0) {
        cur_ev[btn] = ev;
        cur_has_event = true;
        printk("BC: event %s %s (pid %u)\n", btn_name[btn],
               ev == BTHOME_EV_LONG_PRESS ? "long_press" : "press", (uint8_t)(pid + 1));
    }
    pid++;
    pkt_send_t = now;
    pkt_air_t = -1;
    payload_send(now);
}

/* Start a new fader packet: snapshot the live values as the broadcast baseline. */
static void packet_new_faders(int64_t now)
{
    cur_kind = PKT_FADERS;
    cur_has_event = false;
    for (int i = 0; i < 4; i++) {
        cur_fader[i] = fader[i];
        last_bcast_fader[i] = fader[i];
    }
    pid++;
    pkt_send_t = now;
    pkt_air_t = -1;
    payload_send(now);
}

/* True once any fader has moved past the deadband vs. what's been broadcast. */
static bool fader_dirty(void)
{
    if (!fader_seeded) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        int d = (int)fader[i] - (int)last_bcast_fader[i];
        if (d < 0) d = -d;
        if (d > BTHOME_FADER_DEADBAND) {
            return true;
        }
    }
    return false;
}

/* True while any button is (debounced-)down. */
static bool any_down(void)
{
    for (int i = 0; i < BTN_COUNT; i++) {
        if (bthome_clf_down(&clf, i)) {
            return true;
        }
    }
    return false;
}

/* Scan buttons + faders. Debounced edges feed the classifier (tap -> press on
 * release, hold -> long_press at the threshold); resulting events are queued.
 * Fader reads are gated on the rail being unloaded (a held button sags the rail
 * and corrupts them: they freeze at last-good instead). Returns the number of
 * edges seen this tick. */
static int scan_controls(void)
{
    struct button_event evt[BTN_COUNT];
    int n = buttons_scan(evt, BTN_COUNT);
    int64_t now = k_uptime_get();

    for (int i = 0; i < n; i++) {
        printk("BTN %s %s\n", btn_name[evt[i].idx], evt[i].pressed ? "down" : "up");
        uint8_t e = bthome_clf_edge(&clf, evt[i].idx, evt[i].pressed, now);
        if (e != BTHOME_EV_NONE) {
            evq_push(evt[i].idx, e);
        }
    }
    for (int i = 0; i < BTN_COUNT; i++) {      /* long-press threshold crossings */
        uint8_t e = bthome_clf_poll(&clf, i, now);
        if (e != BTHOME_EV_NONE) {
            evq_push((uint8_t)i, e);
        }
    }

    if (!buttons_rail_probe()) {           /* rail clean: faders are trustworthy */
        for (int i = 0; i < 4; i++) {
            int raw = controls_read_raw(2 + i);
            if (raw >= 0) {
                int v = raw * 255 / FADER_RAW_FULL;   /* rail-calibrated 0..255 */
                fader[i] = (uint8_t)(v > 255 ? 255 : v);
            }
        }
        if (!fader_seeded) {               /* first clean read: baseline, not a gesture */
            for (int i = 0; i < 4; i++) {
                last_bcast_fader[i] = fader[i];
            }
            fader_seeded = true;
        }
    }
    return n;
}

/* ---- radio provisioning (CONFIG_SP1_PROVISION): state, sparkle, gesture ---- */

/* True once the radio is KNOWN to be running something other than our beacon
 * app (stock TE = silent, feldd = foreign 0xF0 traffic, older beacon build).
 * Drives the "needs provisioning" chase; cleared by a successful provision
 * or by our app identifying itself on a later wake. */
static bool radio_not_ours;

/* 1: run the chase whenever USB is in, regardless of radio state — a bench
 * visual check of the pattern. Ship with 0. */
#define CHASE_DEMO 1

/* "Radio needs provisioning" cue: all 8 LEDs blink in ONE sequence chasing
 * TOWARDS the PLAY button (top right) — fader LEDs 1→4 (idx 0..3), then the
 * charge LEDs climbing to the full-charge light (idx 4..7), pointing the user
 * at the gesture. While PLAY is held the chase accelerates smoothly with the
 * hold — 150 ms/step down to 35 ms/step across the 5 s consent hold — as live
 * feedback that the gesture is registering. held_ms = 0 when PLAY is up. */
static void chase_tick(int64_t held_ms)
{
    static int64_t frame_t;
    static int step;
    int64_t now = k_uptime_get();

    int64_t period = 150 - (held_ms * 23) / 1000;   /* 150 -> 35 across the 5 s hold */
    if (period < 35) {
        period = 35;                                /* floor lands right at ~5 s */
    }
    if (now - frame_t < period) {
        return;
    }
    frame_t = now;
    step = (step + 1) % LED_COUNT;
    for (int i = 0; i < LED_COUNT; i++) {
        led_idx(i, i == step);
    }
}

#ifdef CONFIG_SP1_PROVISION
static bool radio_probed;    /* probe once per boot (gate or main, whichever first) */

/* One radio probe at boot (USB only): boot the module, listen for its app to
 * identify itself, power it back down. Sets radio_not_ours. */
static void radio_probe(void)
{
    radio_probed = true;
    printk("BT: probing which app the radio runs...\n");
    module_link_power(true);
    int64_t t0 = k_uptime_get();
    while (k_uptime_get() - t0 < 3500 && module_link_app() == 0) {
        feed_wdt();
        module_link_poll();
        k_msleep(10);
    }
    radio_not_ours = (module_link_app() != 1);
    module_link_power(false);
    printk("BT: radio app = %s\n",
           module_link_app() == 1 ? "ours" :
           module_link_app() == -1 ? "OTHER — hold PLAY 5 s (USB in) to provision"
                                   : "silent/stock — hold PLAY 5 s (USB in) to provision");
}

/* Provisioning progress on the 4 side LEDs: PREP = all four blink together;
 * WRITE = a bar filling upward (full quarters solid, the active quarter
 * blinking); VERIFY = the same bar filling back DOWN the other way, so the
 * copy-over and the read-back visibly run in opposite directions.
 * Called from inside the flash engine every chunk, so the blink stays live. */
static void prov_led_progress(enum bt_prov_phase phase, int pct)
{
    bool blink = (k_uptime_get() / 120) & 1;

    if (phase == BT_PROV_PREP) {
        for (int i = 0; i < 4; i++) {
            led_idx(4 + i, blink);
        }
        return;
    }
    int q = pct / 25;                             /* 0..4 full quarters */
    for (int i = 0; i < 4; i++) {
        /* write: bar grows 4->7; verify: mirrored, grows 7->4 */
        int led = (phase == BT_PROV_VERIFY) ? 7 - i : 4 + i;
        led_idx(led, i < q ? true : (i == q ? blink : false));
    }
}
#endif /* CONFIG_SP1_PROVISION */

/* CHARGE-STANDBY GATE (kept from feldd/looper): only a •• wake or a watchdog
 * recovery is a real turn-on; for every other power event, park showing the
 * battery gauge (on USB) or drop to SYSTEM_OFF (on battery) so a full boot can
 * never brown-out-thrash a low cell. Returns only on a real turn-on.
 *
 * The provisioning gesture works while parked here too — a FIRST FLASH boots
 * straight into this gate, and requiring an unplug/power-on/replug dance to
 * reach the flash path would be miserable. The gate samples PLAY off the
 * tracks ladder (rail raised briefly per tick), shows the chase (which
 * overrides the charge gauge) when the radio needs provisioning / CHASE_DEMO /
 * the hold is in progress, and runs the same 5 s gesture. */
static void charge_standby_gate(uint32_t wake_reas)
{
    if (wake_reas & (POWER_RESETREAS_OFF_Msk | POWER_RESETREAS_DOG_Msk)) {
        return;
    }

    int64_t hold_t = -1;
    uint32_t tick = 0;
    int adc_up = 0;
    int last_raw = -1;
#ifdef CONFIG_SP1_PROVISION
    int64_t play_since = -1;   /* uptime PLAY was first seen held, or -1 */
    int64_t play_seen = 0;     /* last tick PLAY read as down (debounce grace) */
    int probed = 0;
    int chasing = 0;
#endif
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
#ifdef CONFIG_SP1_PROVISION
            if (!probed) {                 /* once, on USB: which app is on the radio? */
                probed = 1;
                module_link_init();
                radio_probe();
            }
            int64_t now = k_uptime_get();
            controls_rail(1);              /* sample PLAY off the tracks ladder */
            k_busy_wait(2000);
            int play = buttons_decode_tracks_pure(controls_read_raw(0)) == 0;
            controls_rail(0);
            if (play) {
                if (play_since < 0) {
                    play_since = now;
                }
                play_seen = now;
            } else if (play_since >= 0 && now - play_seen > 150) {
                play_since = -1;           /* released (few-tick bounce grace) */
            }
            bool held = play_since >= 0;
            if (held && now - play_since >= 5000) {
                printk("PROVISION: PLAY held 5 s in charge standby — flashing the radio\n");
                for (int i = 0; i < LED_COUNT; i++) {
                    led_idx(i, false);
                }
                bool ok = bt_provision_run(prov_led_progress);
                for (int i = 0; i < LED_COUNT; i++) {
                    led_idx(i, false);
                }
                for (int n = 0; n < 6; n++) {
                    for (int i = 0; i < 4; i++) {
                        led_idx(ok ? 4 + i : i, n & 1);
                    }
                    feed_wdt();
                    k_msleep(250);
                }
                for (int i = 0; i < LED_COUNT; i++) {
                    led_idx(i, false);
                }
                module_link_init();        /* re-own the UART; module in reset */
                if (ok) {
                    radio_not_ours = false;
                }
                play_since = -1;
            }
            if (radio_not_ours || CHASE_DEMO || held) {
                chasing = 1;
                chase_tick(held ? now - play_since : 0);
            } else {
                if (chasing) {             /* chase just ended: clear its LEDs */
                    chasing = 0;
                    for (int i = 0; i < LED_COUNT; i++) {
                        led_idx(i, false);
                    }
                }
                charge_gauge(battery_pct(last_raw), charging(), tick);
            }
#else
            charge_gauge(battery_pct(last_raw), charging(), tick);
#endif
        }
        k_msleep(40);
        tick++;
    }

    led_set_brightness(LED_BRIGHTNESS_DEFAULT);
    for (int i = 0; i < LED_COUNT; i++) {
        led_idx(i, false);                 /* clear gauge + any chase remnants */
    }
}

static void bc_go_idle(void)
{
    (void)module_link_adv(false);          /* courtesy; the reset kills it anyway */
    module_link_power(false);
    controls_rail(0);
    idle_rail_on = false;                  /* rail dropped; next idle scan re-settles */
    led_pin(SP1_LED1, false);
    evq_head = evq_len = 0;                /* defensive: idle is only reached quiet */
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

#ifdef CONFIG_SP1_PROVISION
    /* On USB power, find out which app the radio runs (drives the "needs
     * provisioning" chase) — unless the charge-standby gate already probed.
     * On battery skip it — a wake identifies the app as a side effect anyway. */
    if (usb_present() && !radio_probed) {
        radio_probe();
    }
#endif

    bthome_clf_init(&clf);
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
            if (!usb_now) {
                controls_rail(0);                          /* battery: drop the rail for power */
                idle_rail_on = false;
            }

            /* Wake on a button (even a not-yet-debounced press on the rail; the
             * SPECIFIC button is left to the debounced scan during the module's
             * boot) — or on a fader moved past the deadband, which gets its own
             * fader packet once the module is up. */
            if (loaded || any_down() || evq_len > 0) {
                bc_wake("button");
            } else if (fader_dirty()) {
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
                radio_not_ours = (module_link_app() == -1);   /* wake re-identifies */
                (void)module_link_adv(true);   /* legacy-app compat; harmless on the beacon app */
                int pct = battery_pct(controls_read_raw(6));   /* one battery read per wake */
                if (pct >= 0) {
                    battery = (uint8_t)pct;
                }
                /* First packet of the session: the first queued event if one
                 * already landed during the boot, else the fader update that
                 * woke us, else an all-none baseline. */
                uint8_t btn, ev;
                if (evq_pop(&btn, &ev)) {
                    packet_new(btn, ev, now);
                } else if (fader_dirty()) {
                    packet_new_faders(now);
                } else {
                    packet_new(-1, BTHOME_EV_NONE, now);
                }
                activity_t = now;              /* linger from the first broadcast, not from wake */
                bc = BC_ON;
            } else if (k_uptime_get() - wake_t > WAKE_TIMEOUT_MS) {
                printk("BC: module boot timeout, retrying via idle\n");
                if (module_link_app() != 1) {
                    radio_not_ours = true;     /* silent radio: stock TE app */
                }
                module_link_power(false);
                bc = BC_IDLE;                  /* keep queued events; re-wake next tick */
                controls_rail(0);
                idle_rail_on = false;
                printk("BC: idle (module boot retry)\n");
            }
        } else if (bc == BC_ON) {
            int64_t now = k_uptime_get();

            bool acked = module_link_last_ack_seq() == (int)pid;
            if (acked && pkt_air_t < 0) {
                pkt_air_t = now;               /* current pid confirmed on the air */
            }
            /* An event packet must stay up for EVENT_DWELL_MS of acked time (or
             * the hard cap) before the next queued event may replace it; a
             * no-event packet may be replaced immediately. */
            bool dwell_done = !cur_has_event ||
                              (pkt_air_t >= 0 && now - pkt_air_t >= EVENT_DWELL_MS) ||
                              (now - pkt_send_t >= EVENT_CAP_MS);
            uint8_t btn, ev;
            if (dwell_done && evq_pop(&btn, &ev)) {
                packet_new(btn, ev, now);      /* next event -> new pid */
                activity_t = now;
            } else if (dwell_done && evq_len == 0 && fader_dirty() &&
                       now - pkt_send_t >= FADER_MIN_MS) {
                packet_new_faders(now);        /* fader update; events always preempt */
                activity_t = now;
            } else if (now - last_send_t >= (acked ? KEEPALIVE_MS : SEND_RETRY_MS)) {
                payload_send(now);             /* same pid + content: retry/keepalive */
            }
            /* Linger restarts while anything is held or events are pending. */
            if (any_down() || evq_len > 0) {
                activity_t = now;
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
                printk("•• tap: bc=%d module=%d pid=%u evq=%d faders=%u/%u/%u/%u batt=%u\n",
                       (int)bc, (int)module_link_state(), pid, evq_len,
                       fader[0], fader[1], fader[2], fader[3], battery);
                if (module_link_state() == MODULE_UP) {
                    (void)module_link_ping();
                }
            }
            func_since = -1;
            func_armed = 1;
        }

#ifdef CONFIG_SP1_PROVISION
        /* CONSENT GESTURE: PLAY held 5 s with USB power in = provision the
         * radio (flash the beacon module app). Explicit opt-in, USB required
         * (never a multi-minute write on a marginal battery). The engine is
         * timeout-bounded and template-gated; on refusal/failure we return
         * here with the •• power-off escape intact. */
        if (usb_now && bthome_clf_down(&clf, 0) &&
            k_uptime_get() - clf.down_t[0] >= 5000) {
            printk("PROVISION: PLAY held 5 s + USB — flashing the radio\n");
            for (int i = 0; i < LED_COUNT; i++) {
                led_idx(i, false);
            }
            bool ok = bt_provision_run(prov_led_progress);
            /* Outcome cue: side LEDs pulse = success; track LEDs pulse = failed
             * (safe to just hold PLAY again to retry). */
            for (int i = 0; i < LED_COUNT; i++) {
                led_idx(i, false);
            }
            for (int n = 0; n < 6; n++) {
                for (int i = 0; i < 4; i++) {
                    led_idx(ok ? 4 + i : i, n & 1);
                }
                feed_wdt();
                k_msleep(250);
            }
            for (int i = 0; i < LED_COUNT; i++) {
                led_idx(i, false);
            }
            module_link_init();          /* re-own the module UART (module in reset) */
            bthome_clf_init(&clf);       /* drop the held-PLAY state + any queue */
            evq_head = evq_len = 0;
            if (ok) {
                radio_not_ours = false;  /* next wake re-confirms via READY */
            }
            bc_go_idle();
        }
#endif /* CONFIG_SP1_PROVISION */

        /* Chase toward PLAY: runs while the radio needs provisioning, AND as
         * live hold feedback whenever the consent gesture is in progress (PLAY
         * held with USB in — even on an already-provisioned radio, since the
         * gesture works there too). Accelerates with the hold. */
        static bool chasing;
        bool play_held = bthome_clf_down(&clf, 0);
        bool chase_on = radio_not_ours || (usb_now && (CHASE_DEMO || play_held));
        if (chase_on) {
            chasing = true;
            chase_tick(play_held ? k_uptime_get() - clf.down_t[0] : 0);
        } else if (chasing) {
            chasing = false;
            for (int i = 0; i < LED_COUNT; i++) {
                led_idx(i, false);
            }
        }

        /* Charge gauge on the 4 side LEDs whenever USB is plugged in — a
         * glanceable charge state while the device is powered on. Battery moves
         * slowly so it's sampled every GAUGE_BATT_MS; the blink is time-based so
         * its rate stays steady across the 8/40 ms loop cadence. */
        if (!chase_on && usb_present()) {
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
