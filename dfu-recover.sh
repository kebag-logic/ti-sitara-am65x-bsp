#!/bin/bash

# SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic
# SPDX-License-Identifier: MIT

# P1 SPL brick-net: recover a no-boot AM62x over USB-C DFU (no JTAG, no media swap). Build DFU-capable bootloaders, set up the DFU host (snagboot + langid fix), and snagrecover the board while it is in USB-DFU boot mode (primary boot-switch B4 ON = bootmode 0x0A; reverts to SD with B4 OFF).
# Usage: dfu-recover.sh {build|host-setup|recover|watch}   env: DFU_HOST=serial-host  DDIR=dfu
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
DFU_HOST="${DFU_HOST:-serial-host}"          # host whose USB-C is wired to the board's OTG port
DDIR="${DDIR:-dfu}"                     # staging dir under the host's $HOME
R5D="$HERE/u-boot-official/out_myir_dfu/r5"; A53D="$HERE/u-boot-official/out_myir_dfu/a53"
LFW="$HERE/ti-linux-firmware"
BL31="$HERE/trusted-firmware-a/build/k3/lite/release/bl31.bin"
TEE="$HERE/optee_os/out/arm-plat-k3/core/tee-raw.bin"

build() {
	# DFU-capable SPLs: the usbdfu fragments add SPL USB-gadget/DFU so each stage can be pushed over USB
	make -C u-boot-official ARCH=arm CROSS_COMPILE=arm-none-eabi- myc_am62x_r5_defconfig am62x_r5_usbdfu.config O="$R5D"
	make -C u-boot-official ARCH=arm CROSS_COMPILE=arm-none-eabi- O="$R5D" BINMAN_INDIRS="$LFW" -j"$(nproc)"
	make -C u-boot-official ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- myc_am62x_a53_defconfig am62x_a53_usbdfu.config O="$A53D"
	make -C u-boot-official ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- O="$A53D" BINMAN_INDIRS="$LFW" BL31="$BL31" TEE="$TEE" -j"$(nproc)"
	echo "built: $R5D/tiboot3-am62x-gp-myc-am62x.bin + $A53D/{tispl.bin,u-boot.img}"
}

host_setup() {
	# install tooling + apply the langid fix: the TI ROM DFU won't return a langid list, so force US-English (0x0409) on the interface-name read
	ssh "$DFU_HOST" 'sudo apt-get install -y dfu-util pipx >/dev/null 2>&1 || true
		pipx install snagboot >/dev/null 2>&1 || true
		f=$(find "$HOME/.local/pipx/venvs/snagboot" -path "*snagrecover/protocols/dfu.py" 2>/dev/null | head -1)
		[ -n "$f" ] && sed -i "s/get_string(dev, intf.iInterface)/get_string(dev, intf.iInterface, 0x0409)/" "$f"
		"$HOME"/.local/bin/snagrecover --version 2>&1 | head -1; echo "langid-patched: $(grep -c 0x0409 "$f")"'
	ssh "$DFU_HOST" "mkdir -p ~/$DDIR"
	scp -q "$R5D/tiboot3-am62x-gp-myc-am62x.bin" "$DFU_HOST:~/$DDIR/tiboot3.bin"
	scp -q "$A53D/tispl.bin" "$DFU_HOST:~/$DDIR/tispl.bin"
	scp -q "$A53D/u-boot.img" "$DFU_HOST:~/$DDIR/u-boot.img"
	ssh "$DFU_HOST" "printf 'tiboot3:\n  path: %s/$DDIR/tiboot3.bin\ntispl:\n  path: %s/$DDIR/tispl.bin\nu-boot:\n  path: %s/$DDIR/u-boot.img\n' \$HOME \$HOME \$HOME > ~/$DDIR/myir-am62x.yaml; echo 'staged ~/$DDIR'"
}

recover() {
	# board must already be in USB-DFU (primary switch B4 ON); pushes tiboot3 -> tispl -> u-boot over USB-C
	ssh "$DFU_HOST" "sudo \$HOME/.local/bin/snagrecover -s am625 -f ~/$DDIR/myir-am62x.yaml"
}

watch() {
	# poll for the board entering USB-DFU, then auto-recover
	ssh "$DFU_HOST" 'for i in $(seq 1 120); do lsusb | grep -qiE "ID 0451:" && break; sleep 2; done; lsusb | grep -iE "0451:"'
	recover
}

case "${1:-recover}" in
	build) build ;;
	host-setup) host_setup ;;
	recover) recover ;;
	watch) watch ;;
	*) echo "Usage: $0 {build|host-setup|recover|watch}  (board in USB-DFU = primary boot-switch B4 ON)"; exit 1 ;;
esac
