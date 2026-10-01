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

/* Built once: sample_usbd_init_device registers the classes and descriptors,
 * which can't be repeated. usbd_disable keeps that state, so a later
 * usbd_enable brings the same device back. */
static struct usbd_context *ctx;

int usbdev_start(void)
{
    if (ctx == NULL) {
        ctx = sample_usbd_init_device(NULL);
        if (ctx == NULL) {
            return -1;   /* no USB; the control loop + WDT still run */
        }
    }
    return usbd_enable(ctx);
}

int usbdev_stop(void)
{
    if (ctx == NULL) {
        return -1;
    }
    return usbd_disable(ctx);
}
