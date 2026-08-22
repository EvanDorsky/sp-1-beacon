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
#   ./scripts/fw.sh flash      flash sp1_beacon.bin over the bootloader (rome, CLI)
#   ./scripts/fw.sh dump       build the read-only full-flash dumper
#   ./scripts/fw.sh monitordump [out.bin]  receive + verify a flash dump
#   ./scripts/fw.sh release    build the PUBLISHABLE image (on-device radio
#                              provisioning compiled in) + run the safety audit
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

# Read-only dump build. Like do_dlbuild but needs NO cybt_blobs.h (pure read).
do_dumpbuild(){ cd "$WS" || exit 1
  ZEPHYR_SDK_INSTALL_DIR="$SDK" west build -b sp1 -d build "$APP" -p -- \
    -DBOARD_ROOT="$BOARD_ROOT" \
    -DEXTRA_CONF_FILE="dump.conf" \
    -DEXTRA_DTC_OVERLAY_FILE="download.overlay"; }

# RELEASE build: the normal beacon firmware + on-device radio provisioning
# (CONFIG_SP1_PROVISION). Needs cybt_blobs.h WITH the SS identity template
# (gen_blobs.py --ss-template). No download.overlay — the runtime DT already
# runs the module UART flow-control-free.
do_release(){ cd "$WS" || exit 1
  [ -f "$APP/src/cybt_blobs.h" ] || {
    echo "missing $APP/src/cybt_blobs.h — generate it first (gen_blobs.py)"; exit 1; }
  grep -q CYBT_HAVE_SS_TEMPLATE "$APP/src/cybt_blobs.h" || {
    echo "cybt_blobs.h lacks the SS template — regenerate with --ss-template <flash_dump.bin>"; exit 1; }
  ZEPHYR_SDK_INSTALL_DIR="$SDK" west build -b sp1 -d build "$APP" -p -- \
    -DBOARD_ROOT="$BOARD_ROOT" \
    -DEXTRA_CONF_FILE="release.conf"; }

# Safety audit on the packaged release image: the binary must be structurally
# incapable of the one unrecoverable act (chip-erase). Tripwires, not proofs —
# the real guarantee is that no chip-erase builder exists in the source (a
# compile-time #error rejects it) — but they catch a regression loudly.
do_audit(){
  NM="$SDK/arm-zephyr-eabi/bin/arm-zephyr-eabi-nm"
  grep -q '^CONFIG_SP1_PROVISION=y' "$BUILD/app/zephyr/.config" || {
    echo "AUDIT FAIL: CONFIG_SP1_PROVISION not set in this build"; exit 1; }
  bad=$("$NM" "$ELF" | grep -iE 'chip_?erase|ss_?write' || true)
  [ -z "$bad" ] && bad_ok=1 || { echo "AUDIT FAIL: forbidden symbols:"; echo "$bad"; exit 1; }
  python3 - "$BIN_OUT" <<'EOF' || exit 1
import sys
data = open(sys.argv[1], 'rb').read()
off = data.find(b'\x01\xce\xff')   # a built CHIP_ERASE H4 command (01, opcode FFCE LE)
if off != -1:
    sys.exit(f"AUDIT FAIL: chip-erase command bytes 01 CE FF at offset {off:#x}")
EOF
  echo "audit OK: provisioning on, no chip-erase symbols, no 01 CE FF in the image"
  shasum -a 256 "$BIN_OUT"; }

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
  monitor) # Poll for the port (up to ~15 s) and attach the instant it appears, so
           # you catch the first log lines after a flash/reset — the flasher's
           # ~2.5 s settle means connecting quickly grabs the banner. Override the
           # wait with a 2nd arg (seconds): ./scripts/fw.sh monitor 30
           secs="${2:-15}"; port=""
           echo "waiting up to ${secs}s for /dev/cu.usbmodem* (reset/plug in the SP-1)..."
           for _ in $(seq 1 $((secs * 5))); do
             port=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
             [ -n "$port" ] && break
             sleep 0.2
           done
           [ -n "$port" ] && { echo "opening $port (ctrl-a k to quit screen)"; screen "$port" 115200; } \
             || echo "no /dev/cu.usbmodem* after ${secs}s - is the SP-1 plugged in and flashed?" ;;
  e2e)     shift
           if command -v uv >/dev/null 2>&1; then uv run "$ROOT/scripts/e2e_monitor.py" "$@"
           else python3 "$ROOT/scripts/e2e_monitor.py" "$@"; fi ;;
  dl)      do_dlbuild "download.conf"     && do_bin ;;   # module flasher, DRY-RUN
  dlarm)   do_dlbuild "download-arm.conf" && do_bin ;;   # module flasher, ARMED write
  dump)    do_dumpbuild && do_bin ;;                     # read-only full-flash dumper
  release) do_release && do_bin && do_audit ;;           # publishable image + audit
  monitordump) shift
           out="${1:-$ROOT/sp1_flash_dump.bin}"
           if command -v uv >/dev/null 2>&1; then uv run "$ROOT/scripts/dump_recv.py" -o "$out"
           else python3 "$ROOT/scripts/dump_recv.py" -o "$out"; fi ;;
  flash)   shift
           # CLI flash over the TE bootloader's serial via rome
           # (github.com/softmodded/rome), the same protocol solderless uses.
           # Put the SP-1 in bootloader mode first: power off, hold Track 1+4,
           # plug USB (or hold Track 1+4 ~1.2 s in-app to DFU-reset into it).
           ROME="${ROME:-$HOME/src/rome/target/release/rome}"
           command -v "$ROME" >/dev/null 2>&1 || [ -x "$ROME" ] || ROME="$(command -v rome)"
           [ -x "$ROME" ] || { echo "rome not found. Build it: (cd ~/src/rome && cargo build --release), or set ROME=/path/to/rome"; exit 1; }
           [ -f "$BIN_OUT" ] || { echo "no $BIN_OUT — run ./scripts/fw.sh bin first"; exit 1; }
           port="${1:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
           [ -n "$port" ] || { echo "no /dev/cu.usbmodem* — is the SP-1 in bootloader mode (Track 1+4 + USB)?"; echo "list ports with: $ROME flash -l"; exit 1; }
           echo "flashing $BIN_OUT via rome on $port"
           "$ROME" flash -p "$port" "$BIN_OUT" ;;
  clean)   rm -rf "$BUILD" && echo "cleaned $BUILD" ;;
  *) sed -n '2,15p' "$0" ;;
esac
