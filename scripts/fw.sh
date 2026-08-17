#!/bin/bash
# SP-1 beacon firmware dev wrapper - one command for build/package/inspect.
# Hides the long west incantation + the sysbuild/SDK/PATH gotchas.
#   ./scripts/fw.sh build      pristine build for the sp1 board
#   ./scripts/fw.sh inc        incremental build (no -p)
#   ./scripts/fw.sh bin        build + produce the flashable sp1_beacon.bin
#   ./scripts/fw.sh info       size + link-address sanity of the current build
#   ./scripts/fw.sh test       run the host unit tests
#   ./scripts/fw.sh monitor    open the SP-1 CDC serial console
#   ./scripts/fw.sh e2e        press->ping E2E monitor (scripts/e2e_monitor.py)
#   ./scripts/fw.sh clean      remove the build dir
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WS="$ROOT/.zephyr-ws"
SDK="$WS/zephyr-sdk-0.17.0"
APP="$ROOT/firmware/app"
BOARD_ROOT="$WS/marisko"                  # sp1 board def (cloned by setup-zephyr-ws.sh)
BUILD="$WS/build"
ELF="$BUILD/app/zephyr/zephyr.elf"        # NCS sysbuild path (note the app/ segment)
BIN_OUT="$APP/sp1_beacon.bin"
export PATH="$HOME/Library/Python/3.14/bin:$SDK/arm-zephyr-eabi/bin:$PATH"
OBJCOPY="$SDK/arm-zephyr-eabi/bin/arm-zephyr-eabi-objcopy"
OBJDUMP="$SDK/arm-zephyr-eabi/bin/arm-zephyr-eabi-objdump"

do_build(){ local p="$1"; cd "$WS" || exit 1
  ZEPHYR_SDK_INSTALL_DIR="$SDK" west build -b sp1 -d build "$APP" $p -- -DBOARD_ROOT="$BOARD_ROOT"; }

# DEV-ONLY module flasher build. $1 = the EXTRA_CONF_FILE (download.conf =
# dry-run, download-arm.conf = armed write).
do_dlbuild(){ local conf="$1"; cd "$WS" || exit 1
  [ -f "$APP/src/cybt_blobs.h" ] || {
    echo "missing $APP/src/cybt_blobs.h — generate it first:"; echo
    echo "  scripts/gen_blobs.py --minidriver <uart.hex> --ds <..._download.hex>"; exit 1; }
  ZEPHYR_SDK_INSTALL_DIR="$SDK" west build -b sp1 -d build "$APP" -p -- \
    -DBOARD_ROOT="$BOARD_ROOT" \
    -DEXTRA_CONF_FILE="$conf" \
    -DEXTRA_DTC_OVERLAY_FILE="download.overlay"; }

do_bin(){
  [ -f "$ELF" ] || { echo "no ELF - run a build first"; exit 1; }
  # The TE bootloader runs the app at 0x20000, so the vector table must link
  # EXACTLY there (CONFIG_FLASH_LOAD_OFFSET=0x20000 + CONFIG_ROM_START_OFFSET=0;
  # the stacked-offsets 0x40000 mistake shipped a dark device once). Hard-gate it.
  vt=$("$OBJDUMP" -t "$ELF" | awk '$NF=="_vector_table"{print $1}')
  [ "$vt" = "00020000" ] || { echo "FATAL: _vector_table at 0x$vt, must be 0x00020000 - NOT packaging"; exit 1; }
  # The image starts at 0x20000, so the objcopy binary already begins with the
  # vector table - no pad to strip.
  "$OBJCOPY" -O binary --gap-fill 0xFF --remove-section=.debug_* --remove-section=.comment \
    --remove-section=.ARM.attributes "$ELF" "$BIN_OUT" || exit 1
  echo "flashable image -> $BIN_OUT  ($(wc -c <"$BIN_OUT") bytes, vector table @0x20000 ok)"
  echo "flash it at https://solderless.engineering (Chrome): enter bootloader (power off, hold track 1+4, plug USB), upload this .bin"
}

do_info(){
  [ -f "$ELF" ] || { echo "no ELF - run a build first"; exit 1; }
  echo "== sections (vector table must be VMA 0x00020000) =="
  "$OBJDUMP" -h "$ELF" | grep -iE 'Idx|vector|text|rom_start' | head
  echo "== size =="; "$SDK/arm-zephyr-eabi/bin/arm-zephyr-eabi-size" "$ELF"
}

case "${1:-}" in
  build)   do_build "-p" ;;
  inc)     do_build "" ;;
  bin)     do_build "-p" && do_bin ;;
  info)    do_info ;;
  test)    cd "$ROOT/firmware/test" && make clean >/dev/null 2>&1; make test ;;
  monitor) port=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
           [ -n "$port" ] && { echo "opening $port (ctrl-a k to quit screen)"; screen "$port" 115200; } \
             || echo "no /dev/cu.usbmodem* - is the SP-1 plugged in and flashed?" ;;
  e2e)     shift
           if command -v uv >/dev/null 2>&1; then uv run "$ROOT/scripts/e2e_monitor.py" "$@"
           else python3 "$ROOT/scripts/e2e_monitor.py" "$@"; fi ;;
  dl)      do_dlbuild "download.conf"     && do_bin ;;   # module flasher, DRY-RUN
  dlarm)   do_dlbuild "download-arm.conf" && do_bin ;;   # module flasher, ARMED write
  clean)   rm -rf "$BUILD" && echo "cleaned $BUILD" ;;
  *) sed -n '2,12p' "$0" ;;
esac
