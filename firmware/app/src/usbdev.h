#ifndef USBDEV_H
#define USBDEV_H
/* USB bring-up (device_next): a CDC-ACM console only.
 *
 * usbdev_start builds the device via the sample_usbd helper on first use and
 * enables it; later calls just re-enable. usbdev_stop disables it. Call
 * usbdev_stop when USB power goes away: an enabled USB driver keeps the
 * 64 MHz crystal oscillator running, which on battery is a few hundred uA of
 * pure idle drain. Both return 0 on success, <0 on error. */
int usbdev_start(void);
int usbdev_stop(void);
#endif
