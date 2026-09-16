# Thin front-end for scripts/fw.sh so the common loop is just `make <target>`.
# The script stays the single source of truth; every target here forwards.
#
#   make            = make release
#   make flash      flash sp1_beacon.bin over the TE bootloader (rome)
#   make release    publishable image (provisioning embedded) + safety audit
#   make bin        DEV image (no radio blob) - beacon only
#   make monitor    open the SP-1 CDC serial console
#   make e2e        BLE press->packet monitor
#   make test       host unit tests
#   make dl/dlarm   dev module flasher (dry-run / ARMED write)
#   make dump       read-only full-flash dumper;  make monitordump receives it
#   make clean      remove the build dir
#
# Extra args pass through: make monitor ARGS=30, make flash ARGS=/dev/cu.usbmodemXXXX

FW := ./scripts/fw.sh
ARGS ?=

TARGETS := build inc bin info test monitor e2e flash dump monitordump release dl dlarm clean

.PHONY: all $(TARGETS) help

all: release

$(TARGETS):
	$(FW) $@ $(ARGS)

help:
	@sed -n '4,16p' $(MAKEFILE_LIST)
