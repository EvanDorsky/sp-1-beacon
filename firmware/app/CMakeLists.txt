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
#   bthome.c                — BTHome v2 payload codec + event classifier (host-tested)
target_sources(app PRIVATE
    src/main.c
    src/buttons.c
    src/controls.c
    src/led.c
    src/usbdev.c
    src/wiced_hci.c
    src/module_link.c
    src/bthome.c
    src/cybt_dl.c)          # pure download-protocol core (host-tested); safe to always build
# DEV-ONLY module flasher + dumper. bt_wire.c is the shared download-mode UART
# layer, compiled for either build. bt_download.c (flasher) #includes the
# generated git-ignored src/cybt_blobs.h; bt_dump.c (read-only dumper) needs no
# blobs. The normal image compiles none of these. DUMP and DOWNLOAD are mutually
# exclusive (Kconfig), so bt_wire.c is added at most once.
target_sources_ifdef(CONFIG_SP1_BT_DOWNLOAD app PRIVATE src/bt_wire.c src/bt_download.c)
target_sources_ifdef(CONFIG_SP1_BT_DUMP     app PRIVATE src/bt_wire.c src/bt_dump.c)
