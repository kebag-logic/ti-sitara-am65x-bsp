#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: MIT

# Build + deploy the TDM8 (8 in / 8 out on McASP1/J11) -> USB Audio Class 2.0
# gadget stack for the MYIR AM6254. Same shape and same guarantees as
# build-am62-kernel.sh: the board's own config is the base, the new kernel and
# the new DTB are added as EXTRA files under a new extlinux label, and the
# entries that boot today are left untouched as a serial-selectable fallback.
#
# Usage: build-tdm8-uac2.sh [fetch|shim|config|dtb|build|stage|deploy|all|image]
#        all   = ... build + deploy over ssh (live board)
#        image = ... build + stage into res/spare-sd/boot for the SD-image builders
# Env:   KVER BOARD JOBS BASE_DTB CROSS_COMPILE
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
KVER="${KVER:-v7.1}"
BOARD="${BOARD:-board}"            # ssh alias (ProxyJump via jump-host)
JOBS="${JOBS:-$(nproc)}"
KSRC="$HERE/linux"
DTB_NAME="k3-am625x-myd-6254-tdm8"
BASE_DTB="${BASE_DTB:-$HERE/res/spare-sd/boot/ti/k3-am625x-myd-6254.dtb}"
OUT_DTB="$HERE/res/tdm8/$DTB_NAME.dtb"
STAGE="$HERE/.kstage-tdm8"
CROSS="${CROSS_COMPILE:-aarch64-linux-gnu-}"
# LOCALVERSION= (set but empty) stops setlocalversion appending "+" for an
# out-of-tag tree, so the release is exactly 7.1.0-tdm8 on every rebuild.
M() { make -C "$KSRC" ARCH=arm64 CROSS_COMPILE="$CROSS" LOCALVERSION= "$@"; }

fetch() {
	[ -f "$KSRC/Makefile" ] || git clone --depth 1 --branch "$KVER" \
		https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git "$KSRC"
}

# Copy the codec shim in and register it in Kconfig/Makefile. Idempotent, so it
# survives a re-clone of ./linux and a kernel-version bump.
shim() { "$HERE/res/tdm8/apply-tdm8-kernel.sh" "$KSRC"; }

config() {
	# Reuse the board's running config, then add only what TDM8+UAC2 needs.
	# Falls back to the snapshot in res/ when the board is not reachable.
	if ssh -o BatchMode=yes "$BOARD" 'zcat /proc/config.gz' > "$KSRC/.config" 2>/dev/null \
	   && [ -s "$KSRC/.config" ]; then
		echo "config: pulled /proc/config.gz from $BOARD"
	else
		echo "config: $BOARD unreachable, using res/board-running.config" >&2
		cp "$HERE/res/board-running.config" "$KSRC/.config"
	fi
	( cd "$KSRC" && ARCH=arm64 ./scripts/kconfig/merge_config.sh -m -O . \
		.config "$HERE/res/kl-tdm8-uac2.config" )
	M olddefconfig
	# olddefconfig only rewrites .config; setlocalversion (and so
	# `make kernelrelease`) reads include/config/auto.conf, which stays at the
	# previous board's value until a build syncs it. Both TDM8 scripts share
	# ./linux, so refresh it here or `stage`/`deploy` name things wrongly.
	M syncconfig
	echo "kernelrelease=$(M -s kernelrelease)"
	for s in CONFIG_SND_SOC_KL_TDM8_DUMMY CONFIG_SND_SIMPLE_CARD \
	         CONFIG_SND_SOC_DAVINCI_MCASP CONFIG_USB_F_UAC2 CONFIG_USB_CONFIGFS; do
		grep -q "^$s=[ym]" "$KSRC/.config" || { echo "$s did not survive olddefconfig" >&2; exit 1; }
	done
}

# Inject the TDM8 nodes into the vendor blob. Never edits the vendor DTB.
dtb() { python3 "$HERE/res/tdm8/mk-tdm8-dtb.py" "$BASE_DTB" "$OUT_DTB"; }

build() {
	M -j"$JOBS" Image.gz modules
	rm -rf "$STAGE"
	M INSTALL_MOD_PATH="$STAGE" INSTALL_MOD_STRIP=1 modules_install
	ls -l "$STAGE"/lib/modules/*/kernel/sound/soc/codecs/snd-soc-kl-tdm8-dummy.ko*
}

# Stage the boot artifacts into res/spare-sd/boot/ so the SD-image builders
# (build-spare-sd.sh) pick up the TDM8 kernel + device tree. Non-destructive:
# every kernel, DTB and extlinux label already staged there is preserved; we
# only add the TDM8 pair and make it the default. Re-running replaces our own
# labels rather than stacking duplicates.
stage() {
	local rel boot append existing
	rel=$(M -s kernelrelease)
	boot="$HERE/res/spare-sd/boot"
	[ -f "$OUT_DTB" ] || { echo "run '$0 dtb' first" >&2; exit 1; }
	[ -f "$KSRC/arch/arm64/boot/Image.gz" ] || { echo "run '$0 build' first" >&2; exit 1; }

	mkdir -p "$boot/ti" "$boot/extlinux"
	cp "$KSRC/arch/arm64/boot/Image.gz" "$boot/Image-$rel.gz"
	cp "$OUT_DTB" "$boot/ti/$DTB_NAME.dtb"

	# reuse the cmdline the staged config already uses; fall back to the one the
	# board actually boots with (see README.install-kernel.md)
	# NB: grep -m1 stops per FILE, so two files yield two lines - head -1 after.
	append=$(grep -h 'append' "$boot/extlinux/extlinux.conf" \
		"$boot/extlinux/extlinux.conf.orig" 2>/dev/null |
		head -1 | sed 's/^[[:space:]]*append //')
	[ -n "$append" ] || append="console=ttyS2,115200n8 earlycon=ns16550a,mmio32,0x02800000 root=/dev/mmcblk1p2 ro rootfstype=ext4 rootwait net.ifnames=0"

	# everything from the first "label" onward, minus labels we own
	existing=$(awk '/^label /{ keep = ($2 != "tdm8" && $2 != "notdm8") } keep' \
		"$boot/extlinux/extlinux.conf" 2>/dev/null || true)

	{
		echo "menu title MYIR AM62x microSD (TDM8 -> UAC2)"
		echo "timeout 30"
		echo "prompt 1"
		echo "default tdm8"
		echo "label tdm8"
		echo "    menu label Linux $rel + TDM8 on McASP1/J11"
		echo "    kernel /Image-$rel.gz"
		echo "    fdtdir /"
		echo "    fdt /ti/$DTB_NAME.dtb"
		echo "    append $append"
		# same kernel, stock device tree: isolates a TDM8 DT problem from a
		# kernel problem without reflashing
		if [ -f "$boot/ti/k3-am625x-myd-6254.dtb" ]; then
			echo "label notdm8"
			echo "    menu label Linux $rel, stock device tree (TDM8 off)"
			echo "    kernel /Image-$rel.gz"
			echo "    fdtdir /"
			echo "    fdt /ti/k3-am625x-myd-6254.dtb"
			echo "    append $append"
		fi
		[ -n "$existing" ] && echo "$existing"
	} > "$boot/extlinux/extlinux.conf.new"
	mv "$boot/extlinux/extlinux.conf.new" "$boot/extlinux/extlinux.conf"

	echo "staged into $boot:"
	echo "  Image-$rel.gz"
	echo "  ti/$DTB_NAME.dtb"
	echo "  extlinux/extlinux.conf  (default=tdm8; kept: $(echo "$existing" | grep -c '^label ') pre-existing label(s))"
	echo "next: KIMG_NAME=Image-$rel.gz ./build-spare-sd.sh"
}

deploy() {
	local rel; rel=$(M -s kernelrelease)
	[ -f "$OUT_DTB" ] || { echo "run '$0 dtb' first" >&2; exit 1; }
	tar -C "$STAGE/lib/modules" -czf /tmp/kmods-"$rel".tgz "$rel"
	scp "$KSRC/arch/arm64/boot/Image.gz" "$BOARD":/tmp/Image-"$rel".gz
	scp /tmp/kmods-"$rel".tgz "$BOARD":/tmp/
	scp "$OUT_DTB" "$BOARD":/tmp/"$DTB_NAME".dtb
	ssh "$BOARD" "rel='$rel' DTB='$DTB_NAME' sh -s" <<'REMOTE'
set -e
# the board's tar is BusyBox (no -z); modules go to a versioned dir so kernels coexist
gzip -dc /tmp/kmods-"$rel".tgz | tar -C /lib/modules -xf -
depmod "$rel" 2>/dev/null || true
mkdir -p /mnt/bootp; mount /dev/mmcblk1p1 /mnt/bootp
cp /tmp/Image-"$rel".gz /mnt/bootp/Image-"$rel".gz
cp /tmp/"$DTB".dtb /mnt/bootp/ti/"$DTB".dtb
[ -f /mnt/bootp/extlinux/extlinux.conf.orig ] || \
	cp /mnt/bootp/extlinux/extlinux.conf /mnt/bootp/extlinux/extlinux.conf.orig
A=$(grep -m1 'append' /mnt/bootp/extlinux/extlinux.conf.orig | sed 's/^[[:space:]]*append //')
cat > /mnt/bootp/extlinux/extlinux.conf <<EXL
menu title MYIR AM62x microSD
timeout 30
prompt 1
default tdm8
label tdm8
    kernel /Image-$rel.gz
    fdtdir /
    fdt /ti/$DTB.dtb
    append $A
label rebuilt
    kernel /Image-$rel.gz
    fdtdir /
    fdt /ti/k3-am625x-myd-6254.dtb
    append $A
label linux
    kernel /Image.gz
    fdtdir /
    fdt /ti/k3-am625x-myd-6254.dtb
    append $A
EXL
sync; umount /mnt/bootp
echo "deployed $rel + $DTB.dtb"
echo "enable it with: sed -i 's/^TDM8_ENABLE=no/TDM8_ENABLE=yes/' /etc/tdm8/tdm8.env"
echo "then reboot; the serial console can still pick 'rebuilt' or 'linux'"
REMOTE
}

case "${1:-all}" in
fetch)  fetch ;;
shim)   fetch; shim ;;
config) fetch; shim; config ;;
dtb)    dtb ;;
build)  shim; build ;;
stage)  stage ;;
deploy) deploy ;;
all)    fetch; shim; config; dtb; build; deploy ;;
image)  fetch; shim; config; dtb; build; stage ;;
*) echo "Usage: $0 {fetch|shim|config|dtb|build|stage|deploy|all|image}"; exit 1 ;;
esac
