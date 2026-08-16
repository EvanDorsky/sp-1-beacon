/*
 * usbdev.c — USB bring-up for the SP-1 beacon firmware: a CDC-ACM console only.
 *
 * The single device_next USBD context is built by the vendored sample_usbd
 * helper; usbd_register_all_classes() picks up the cdc_acm_uart0 node
 * (CONFIG_USBD_CDC_ACM_CLASS) and the console rides it (chosen zephyr,console
 * in app.overlay). feldd's USB-MIDI + HID functions are gone.
 */
#include "usbdev.h"
#include <zephyr/usb/usbd.h>
#include <sample_usbd.h>

int usbdev_start(void)
{
    struct usbd_context *ctx = sample_usbd_init_device(NULL);
    if (ctx == NULL) {
        return -1;   /* no USB; the control loop + WDT still run */
    }
    return usbd_enable(ctx);
}
