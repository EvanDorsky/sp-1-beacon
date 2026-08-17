cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sp1-beacon)
# Pulls in the sample_usbd device_next bring-up helper (sample_usbd_init.c +
# its include dir) used for the USB-CDC console. Mirrors the looper/feldd.
include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)
# The beacon firmware proper:
#   buttons.c / controls.c  — the SP-1's 2 ladder ADCs -> 9 debounced buttons
#                             (kept from feldd, host-tested in firmware/test)
#   led.c                   — 8-LED PWM drive (kept from feldd)
#   usbdev.c                — CDC-ACM console bring-up
#   wiced_hci.c             — pure WICED-HCI wire codec (host-tested)
#   module_link.c           — CYW20706 runtime link: reset sequencing + UART0
#   beacon_state.c          — the state-beacon payload codec (host-tested)
target_sources(app PRIVATE
    src/main.c
    src/buttons.c
    src/controls.c
    src/led.c
    src/usbdev.c
    src/wiced_hci.c
    src/module_link.c
    src/beacon_state.c)
