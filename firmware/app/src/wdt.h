#ifndef SP1_WDT_H
#define SP1_WDT_H

/* Reload (feed) the hardware watchdog. Defined in main.c.
 *
 * Any long SYNCHRONOUS work that runs outside the control loop's per-tick feed
 * (boot-time waits, the charge-standby park, the •• release wait before
 * SYSTEM_OFF) must call this so it cannot trip the ~8 s WDT. */
void feed_wdt(void);

#endif /* SP1_WDT_H */
