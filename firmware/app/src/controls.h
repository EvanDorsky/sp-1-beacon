#ifndef CONTROLS_H
#define CONTROLS_H
int controls_init(void);
int controls_read_raw(int idx);   /* idx per zephyr_user io-channels: 0=tracks
                                   * ladder, 1=vol ladder, 2..5=faders 1..4,
                                   * 6=battery; -1 on error, else 0..4095 */
/* Drive the BTN_COM supply rail (P1.10). Everything controls_read_raw samples
 * is powered by it; the idle loop duty-cycles it (on -> settle -> read -> off)
 * so the fader pots don't drain the cell around the clock. Reads with the rail
 * low return garbage — the caller owns the sequencing. */
void controls_rail(int on);
#endif
