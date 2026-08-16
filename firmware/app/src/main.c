/*
 * main.c — SP-1 BLE beacon firmware (M1+M2 bring-up build).
 *
 * A stripped-down descendant of feldd's controller firmware: the SP-1's 9
 * buttons drive the onboard CYW20706 Bluetooth module (running the feldd BLE
 * module app) over WICED-HCI. This build's job is the bench bring-up:
 *
 *   - M1: buttons scan + debounce (ladder ADC), every edge logged on the
 *     USB-CDC console, with the boot-safety conventions kept intact (links
 *     above the TE bootloader, Track1+4 DFU escape, charge-standby gate,
 *     charger /CE enable, •• 5 s hold = power off).
 *   - M2: drive the module's flashed app: PLAY = PING, Track1..4 = a ~2 s
 *     advertising burst (presence beacon), VolUp/VolDn = ADV on/off,
 *     FWD/RWD = module boot / module reset. All module frames are hex-logged.
 *
 * Button map (BTN_COUNT indices from buttons.h):
 *   0 PLAY  -> PING          1..4 Track1..4 -> ADV burst (~2 s)
 *   5 VolUp -> ADV on        6 VolDn        -> ADV off
 *   7 FWD   -> module on     8 RWD          -> module off (reset)
 *   ••  5 s hold -> power off (SYSTEM_OFF; •• press wakes)
 *   Track1+4 held ~1.2 s -> DFU (bootloader re-flash escape hatch)
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
#include "wdt.h"

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
    nrf_gpio_cfg_output(SP1_BTN_COM);   /* stop powering the ladders */
    nrf_gpio_pin_clear(SP1_BTN_COM);

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

/* ---- FAILSAFE recovery (Track1+4 held ~1.2 s): reset into the TE bootloader
 * so the device can ALWAYS be reflashed. Verbatim from feldd/looper. ---- */

static void enter_dfu(void)
{
    led_pin(SP1_TRACK_LED1, true);
    led_pin(SP1_TRACK_LED2, true);
    led_pin(SP1_TRACK_LED3, true);
    led_pin(SP1_TRACK_LED4, true);
    NRF_POWER->GPREGRET = 0x57u;
    __DSB();
    NVIC_SystemReset();
    for (;;) { }
}

/* ---- battery gauge for the charge-standby park (calibration from feldd) ---- */

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

/* CHARGE-STANDBY GATE (kept from feldd, which mirrors the looper): the TE
 * bootloader hands us control on ANY power event — •• power-on, but also a bare
 * USB plug-in or a battery insert. Only a •• wake (RESETREAS.OFF) or a watchdog
 * recovery (RESETREAS.DOG) is a real turn-on; for everything else, park showing
 * the battery gauge (on USB) or drop to SYSTEM_OFF (on battery) so a full boot
 * can never brown-out-thrash a low cell. Returns only on a real turn-on. */
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
                last_raw = controls_read_raw(2);      /* battery */
                adc_up = 1;
            } else if ((tick % 50u) == 0u) {
                last_raw = controls_read_raw(2);
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

/* ---- M2 behavior: buttons -> module commands ---- */

#define TICK_MS         8
#define ADV_BURST_TICKS (2000 / TICK_MS)       /* ~2 s presence-beacon burst */
#define FUNC_OFF_TICKS  (5000 / TICK_MS)       /* •• hold-to-power-off */

static const char *const btn_name[BTN_COUNT] = {
    "PLAY", "T1", "T2", "T3", "T4", "VOL+", "VOL-", "FWD", "RWD",
};

static int      adv_ticks;      /* >0: burst in flight, counts down */
static int      adv_track;      /* which track LED is lit for the burst, -1 none */
static bool     adv_pending;    /* burst requested while the module was booting */
static bool     ping_pending;   /* ping requested while the module was booting */

static void adv_burst_start(int track_idx)
{
    if (module_link_state() != MODULE_UP) {
        adv_pending = true;
        module_link_power(true);        /* boots the app; burst fires on UP */
        return;
    }
    (void)module_link_adv(true);
    adv_ticks = ADV_BURST_TICKS;
    if (adv_track >= 0) {
        led_idx(adv_track, false);
    }
    adv_track = track_idx;
    if (adv_track >= 0) {
        led_idx(adv_track, true);
    }
}

static void adv_burst_tick(void)
{
    /* Work asked for before the app was up fires as soon as it is. */
    if (ping_pending && module_link_state() == MODULE_UP) {
        ping_pending = false;
        (void)module_link_ping();
    }
    if (adv_pending && module_link_state() == MODULE_UP) {
        adv_pending = false;
        adv_burst_start(adv_track >= 0 ? adv_track : 0);
    }
    if (adv_ticks > 0 && --adv_ticks == 0) {
        (void)module_link_adv(false);
        if (adv_track >= 0) {
            led_idx(adv_track, false);
            adv_track = -1;
        }
    }
}

static void handle_button(const struct button_event *e)
{
    printk("BTN %s %s\n", btn_name[e->idx], e->pressed ? "down" : "up");
    if (!e->pressed) {
        return;
    }
    /* E2E liveness: EVERY press (except RWD, the module-off switch) pings the
     * module app, booting it first if needed. The app's reply (its READY event)
     * closes the button -> nRF -> module -> nRF loop on the console, which
     * scripts/e2e_monitor.py watches for. */
    if (e->idx != 8) {
        if (module_link_state() == MODULE_UP) {
            (void)module_link_ping();
        } else {
            ping_pending = true;
            module_link_power(true);          /* no-op unless the module is off */
        }
    }
    switch (e->idx) {
    case 0:                                   /* PLAY: ping only (above) */
        break;
    case 1: case 2: case 3: case 4:           /* Track N: presence-beacon burst */
        adv_track = e->idx - 1;
        adv_burst_start(e->idx - 1);
        break;
    case 5:                                   /* Vol+: advertising on (manual) */
        (void)module_link_adv(true);
        break;
    case 6:                                   /* Vol-: advertising off */
        (void)module_link_adv(false);
        break;
    case 7:                                   /* FWD: boot the module */
        module_link_power(true);
        break;
    case 8:                                   /* RWD: module into reset */
        module_link_power(false);
        adv_ticks = 0;
        adv_pending = false;
        ping_pending = false;
        if (adv_track >= 0) {
            led_idx(adv_track, false);
            adv_track = -1;
        }
        break;
    default:
        break;
    }
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

    charge_standby_gate(wake_reas);

    boot_signature();

    controls_init();
    buttons_init();
    usbdev_start();             /* USB-CDC console */
    module_link_init();         /* UART up; module stays in reset until asked */

    printk("sp1-beacon M1/M2 bring-up (wake=%08x)\n", wake_reas);

    uint32_t func_held = 0;
    int      func_armed = 0;    /* honor •• only after it has read released once */
    adv_track = -1;

    for (;;) {
        feed_wdt();

        struct button_event evt[BTN_COUNT];
        int n = buttons_scan(evt, BTN_COUNT);
        for (int i = 0; i < n; i++) {
            handle_button(&evt[i]);
        }

        if (buttons_dfu_held()) {
            printk("DFU combo: rebooting into the bootloader\n");
            enter_dfu();
        }

        /* •• long-hold = power off (short taps just log; the tap band is free
         * for future gestures). */
        if (nrf_gpio_pin_read(SP1_FUNC_BTN) == 0) {
            if (func_armed && ++func_held >= FUNC_OFF_TICKS) {
                printk("•• held: powering off\n");
                power_off();
            }
        } else {
            if (func_held > 0 && func_held < FUNC_OFF_TICKS / 4) {
                printk("•• tap (module=%d)\n", (int)module_link_state());
            }
            func_held = 0;
            func_armed = 1;
        }

        module_link_poll();
        adv_burst_tick();

        /* Side LED 4 mirrors the module: on = app UP, off = in reset. */
        led_pin(SP1_LED1, module_link_state() == MODULE_UP);

        k_msleep(TICK_MS);
    }
    return 0;
}
