#!/bin/bash

# SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic
# SPDX-License-Identifier: MIT

# Build + deploy the MYIR AM62x mainline kernel: reuse the board's own config, cross-compile, deploy via extlinux
# Usage: build-am62-kernel.sh [fetch|shim|config|build|deploy|all]   env: KVER BOARD JOBS DTB
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
KVER="${KVER:-v7.1}"          # latest stable 7.x mainline (6.19-rc became v7.0, then v7.1)
BOARD="${BOARD:-board}"   # ssh alias (ProxyJump via jump-host)
JOBS="${JOBS:-$(nproc)}"
DTB="${DTB:-k3-am625x-myd-6254}"
KSRC="$HERE/linux"
M() { make -C "$KSRC" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- "$@"; }

fetch() {
	# Mainline has no MYIR DTS; clone the kernel into ./linux (the .gitmodules url is a self-ref placeholder)
	[ -f "$KSRC/Makefile" ] || git clone --depth 1 --branch "$KVER" \
		https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git "$KSRC"
}

# res/tdm8/patches/ carries the SoC-level kernel fixes this BSP needs on any
# AM62x - the BCDMA cyclic-RX one in particular - and the same script stages
# them for the three TDM8 builds. It brings the TDM8 codec shim along, which
# costs a plain build nothing: SND_SOC_KL_TDM8_DUMMY is a new symbol, so
# olddefconfig leaves it off unless a config asks for it.
shim() { "$HERE/res/tdm8/apply-tdm8-kernel.sh" "$KSRC"; }

config() {
	# Reuse the board's running config, then adapt it to this kernel version
	ssh "$BOARD" 'zcat /proc/config.gz' > "$KSRC/.config"
	M olddefconfig
	echo "kernelrelease=$(M -s kernelrelease)"
}

build() {
	M -j"$JOBS" Image.gz modules
	rm -rf "$HERE/.kstage"
	M INSTALL_MOD_PATH="$HERE/.kstage" INSTALL_MOD_STRIP=1 modules_install
}

deploy() {
	# Reuse the working on-SD DTB (/ti/$DTB.dtb already present); only the Image + modules change
	local rel; rel=$(M -s kernelrelease)
	tar -C "$HERE/.kstage/lib/modules" -czf /tmp/kmods-"$rel".tgz "$rel"
	scp "$KSRC/arch/arm64/boot/Image.gz" "$BOARD":/tmp/Image-"$rel".gz
	scp /tmp/kmods-"$rel".tgz "$BOARD":/tmp/
	ssh "$BOARD" "rel='$rel' DTB='$DTB' sh -s" <<'REMOTE'
set -e
# board tar is BusyBox (no -z) -> gzip|tar; clean-extract into a versioned modules dir
gzip -dc /tmp/kmods-"$rel".tgz | tar -C /lib/modules -xf -
mkdir -p /mnt/bootp; mount /dev/mmcblk1p1 /mnt/bootp
cp /tmp/Image-"$rel".gz /mnt/bootp/Image-"$rel".gz
[ -f /mnt/bootp/extlinux/extlinux.conf.orig ] || cp /mnt/bootp/extlinux/extlinux.conf /mnt/bootp/extlinux/extlinux.conf.orig
# preserve the original kernel cmdline; add a 'rebuilt' default, keep the original Image.gz as 'linux' fallback
A=$(grep -m1 'append' /mnt/bootp/extlinux/extlinux.conf.orig | sed 's/^[[:space:]]*append //')
cat > /mnt/bootp/extlinux/extlinux.conf <<EXL
menu title MYIR AM62x microSD
timeout 30
prompt 1
default rebuilt
label rebuilt
    kernel /Image-$rel.gz
    fdtdir /
    fdt /ti/$DTB.dtb
    append $A
label linux
    kernel /Image.gz
    fdtdir /
    fdt /ti/$DTB.dtb
    append $A
EXL
sync; umount /mnt/bootp
echo "deployed $rel; reboot to boot it (serial can pick the 'linux' fallback)"
REMOTE
}

case "${1:-all}" in
	fetch)  fetch ;;
	shim)   fetch; shim ;;
	config) fetch; shim; config ;;
	build)  shim; build ;;
	deploy) deploy ;;
	all)    fetch; shim; config; build; deploy ;;
	*) echo "Usage: $0 {fetch|shim|config|build|deploy|all}"; exit 1 ;;
esac
