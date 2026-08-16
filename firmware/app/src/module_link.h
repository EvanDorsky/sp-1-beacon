#ifndef MODULE_LINK_H
#define MODULE_LINK_H
/*
 * module_link.h — runtime link to the SP-1's CYW20706 Bluetooth module.
 *
 * Owns the module's reset line and the WICED-HCI UART, in NORMAL (app-running)
 * mode only: it boots the module's flashed BLE app and drives its private
 * command group (PING / ADV). It contains NO download-mode machinery — nothing
 * here can write the module's flash.
 *
 * Wiring + boot-strap rules (bluetooth/hardware-and-architecture.md §1, §5):
 *   - UART0: nRF TX P1.02 -> module RX, nRF RX P1.04 <- module TX, 115200 8N1.
 *   - nRF CTS P1.03 <- module RTS: pselled as real UARTE flow control.
 *   - nRF "RTS" P1.01 -> module CTS: driven as a PLAIN GPIO, never pselled.
 *     Holding it LOW while module reset releases is the DOWNLOAD-MODE strap, so
 *     it must be HIGH across every normal boot and only dropped afterwards to
 *     let the module transmit.
 *   - Module RST_N on nRF P0.10, active low. Held low = module off (BT off).
 */
#include <stdbool.h>
#include <stdint.h>

enum module_state {
    MODULE_OFF = 0,     /* held in reset */
    MODULE_BOOTING,     /* reset released, waiting for the app's first frame */
    MODULE_UP,          /* saw a FELDD-group event (READY) — app is alive */
};

int  module_link_init(void);          /* claim pins/UART; module stays in reset */

void module_link_power(bool on);      /* release / assert the module's reset */
enum module_state module_link_state(void);

/* Drain module RX through the codec, log every frame, advance BOOTING->UP.
 * Call every control-loop tick. */
void module_link_poll(void);

/* Send a FELDD-group command (wiced_hci.h WHCI_FELDD_*). Returns 0, or -1 if
 * the module is not powered. */
int  module_link_send(uint8_t code, const uint8_t *payload, uint16_t len);

static inline int module_link_ping(void)
{
    return module_link_send(0x03 /* WHCI_FELDD_PING */, 0, 0);
}
int  module_link_adv(bool enable);

#endif /* MODULE_LINK_H */
