#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: MIT

# Build + deploy the TDM8 -> USB Audio Class 2.0 gadget stack for the
# TI SK-AM62B-P1 (AM625 Starter Kit).  Sibling of build-tdm8-uac2.sh, which does
# the same job for the MYIR AM6254; the two share the codec shim
# (res/tdm8/kl-tdm8-dummy.c) and the kernel fragment (res/kl-tdm8-uac2.config).
#
# The one real difference: mainline carries k3-am625-sk.dts, so the device trees
# are ordinary in-tree sources that #include it, not an injection into a vendor
# blob.  Two of them are built:
#
#   k3-am625-sk-tdm8.dtb     McASP1, 8 in + 8 out   (default)
#   k3-am625-sk-tdm8-j3.dtb  McASP0 on the 40-pin header J3, 8 in only
#
# Both are staged, and the stock k3-am625-sk.dtb is kept as a third extlinux
# entry, so a bad TDM8 tree is one serial-console keystroke away from a
# working boot.  See README.tdm8-sk-am62b.md.
#
# Usage: build-tdm8-uac2-sk.sh [fetch|shim|dts|config|dtb|build|stage|deploy|all|image]
#        all   = ... build + deploy over ssh (live board)
#        image = ... build + stage into res/spare-sd-sk/boot for build-spare-sd.sh
# Env:   KVER BOARD JOBS CROSS_COMPILE
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
KVER="${KVER:-v7.1}"
BOARD="${BOARD:-sk}"               # ssh alias of a live SK-AM62B-P1
JOBS="${JOBS:-$(nproc)}"
KSRC="$HERE/linux"
DTB_NAME="k3-am625-sk-tdm8"        # McASP1, 8x8
DTB_J3_NAME="k3-am625-sk-tdm8-j3"  # McASP0 on J3, capture only
DTB_STOCK="k3-am625-sk"            # untouched mainline SK tree, the fallback
OUTDIR="$HERE/res/tdm8-sk"
STAGE="$HERE/.kstage-tdm8-sk"
BOOTDIR="$HERE/res/spare-sd-sk/boot"
CROSS="${CROSS_COMPILE:-aarch64-linux-gnu-}"
# LOCALVERSION= (set but empty) stops setlocalversion appending "+" for an
# out-of-tag tree, so the release is exactly 7.1.0-tdm8-sk on every rebuild.
M() { make -C "$KSRC" ARCH=arm64 CROSS_COMPILE="$CROSS" LOCALVERSION= "$@"; }

fetch() {
	[ -f "$KSRC/Makefile" ] || git clone --depth 1 --branch "$KVER" \
		https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git "$KSRC"
}

# The codec shim is board-independent - same file, same Kconfig/Makefile hooks
# as the MYIR build.  Idempotent, so running both boards' scripts is fine.
shim() { "$HERE/res/tdm8/apply-tdm8-kernel.sh" "$KSRC"; }

# Copy the two SK device trees in and register them in ti/Makefile.
dts() { "$HERE/res/tdm8-sk/apply-tdm8-sk-dts.sh" "$KSRC"; }

config() {
	# A live SK's own config if there is one, otherwise plain arm64 defconfig -
	# which already carries the whole K3 base (see res/kl-sk-am62b.config).
	if ssh -o BatchMode=yes "$BOARD" 'zcat /proc/config.gz' > "$KSRC/.config" 2>/dev/null \
	   && [ -s "$KSRC/.config" ]; then
		echo "config: pulled /proc/config.gz from $BOARD"
	else
		echo "config: $BOARD unreachable, starting from arm64 defconfig" >&2
		rm -f "$KSRC/.config"
		M defconfig
	fi
	( cd "$KSRC" && ARCH=arm64 ./scripts/kconfig/merge_config.sh -m -O . \
		.config "$HERE/res/kl-tdm8-uac2.config" "$HERE/res/kl-sk-am62b.config" )
	M olddefconfig
	# olddefconfig only rewrites .config; setlocalversion (and so
	# `make kernelrelease`) reads include/config/auto.conf, which stays at the
	# previous board's value until a build syncs it. Both TDM8 scripts share
	# ./linux, so refresh it here or `stage`/`deploy` name things wrongly.
	M syncconfig
	echo "kernelrelease=$(M -s kernelrelease)"
	for s in CONFIG_SND_SOC_KL_TDM8_DUMMY CONFIG_SND_SIMPLE_CARD \
	         CONFIG_SND_SOC_DAVINCI_MCASP CONFIG_USB_F_UAC2 CONFIG_USB_CONFIGFS \
	         CONFIG_USB_DWC3_AM62; do
		grep -q "^$s=[ym]" "$KSRC/.config" || { echo "$s did not survive olddefconfig" >&2; exit 1; }
	done
	# these three decide whether the board can reach its own rootfs at all
	for s in CONFIG_MMC_SDHCI_AM654 CONFIG_GPIO_PCA953X CONFIG_REGULATOR_GPIO; do
		grep -q "^$s=y" "$KSRC/.config" || { echo "$s must be =y (microSD power path)" >&2; exit 1; }
	done
}

# Build both TDM8 trees plus the stock SK tree used by the fallback entry.
dtb() {
	[ -f "$KSRC/.config" ] || { echo "run '$0 config' first" >&2; exit 1; }
	[ -f "$KSRC/arch/arm64/boot/dts/ti/$DTB_NAME.dts" ] || { echo "run '$0 dts' first" >&2; exit 1; }
	M "ti/$DTB_NAME.dtb" "ti/$DTB_J3_NAME.dtb" "ti/$DTB_STOCK.dtb"
	mkdir -p "$OUTDIR"
	for d in "$DTB_NAME" "$DTB_J3_NAME" "$DTB_STOCK"; do
		cp "$KSRC/arch/arm64/boot/dts/ti/$d.dtb" "$OUTDIR/$d.dtb"
	done
	ls -l "$OUTDIR"/*.dtb
}

build() {
	M -j"$JOBS" Image.gz modules
	rm -rf "$STAGE"
	M INSTALL_MOD_PATH="$STAGE" INSTALL_MOD_STRIP=1 modules_install
	ls -l "$STAGE"/lib/modules/*/kernel/sound/soc/codecs/snd-soc-kl-tdm8-dummy.ko*
}

# Stage the boot artifacts into res/spare-sd-sk/boot/ so build-spare-sd.sh can
# make a card out of them.  Non-destructive: anything already staged there is
# kept as a fallback entry, and re-running replaces our own labels rather than
# stacking duplicates.
stage() {
	local rel append existing
	rel=$(M -s kernelrelease)
	for d in "$DTB_NAME" "$DTB_J3_NAME" "$DTB_STOCK"; do
		[ -f "$OUTDIR/$d.dtb" ] || { echo "run '$0 dtb' first" >&2; exit 1; }
	done
	[ -f "$KSRC/arch/arm64/boot/Image.gz" ] || { echo "run '$0 build' first" >&2; exit 1; }

	mkdir -p "$BOOTDIR/ti" "$BOOTDIR/extlinux"
	cp "$KSRC/arch/arm64/boot/Image.gz" "$BOOTDIR/Image-$rel.gz"
	for d in "$DTB_NAME" "$DTB_J3_NAME" "$DTB_STOCK"; do
		cp "$OUTDIR/$d.dtb" "$BOOTDIR/ti/$d.dtb"
	done

	# reuse whatever cmdline is already staged; otherwise the SK default:
	# main_uart0 = ttyS2 @ 0x02800000, rootfs on the microSD (sdhci1 = mmcblk1)
	# NB: grep -m1 stops per FILE, so two files yield two lines - head -1 after.
	append=$(grep -h 'append' "$BOOTDIR/extlinux/extlinux.conf" \
		"$BOOTDIR/extlinux/extlinux.conf.orig" 2>/dev/null |
		head -1 | sed 's/^[[:space:]]*append //')
	[ -n "$append" ] || append="console=ttyS2,115200n8 earlycon=ns16550a,mmio32,0x02800000 root=/dev/mmcblk1p2 ro rootfstype=ext4 rootwait net.ifnames=0"

	# everything from the first "label" onward, minus labels we own
	existing=$(awk '/^label /{ keep = ($2 != "tdm8" && $2 != "tdm8-j3" && $2 != "notdm8") } keep' \
		"$BOOTDIR/extlinux/extlinux.conf" 2>/dev/null || true)

	{
		echo "menu title TI SK-AM62B-P1 microSD (TDM8 -> UAC2)"
		echo "timeout 30"
		echo "prompt 1"
		echo "default tdm8"
		echo "label tdm8"
		echo "    menu label Linux $rel + TDM8 8x8 on McASP1"
		echo "    kernel /Image-$rel.gz"
		echo "    fdtdir /"
		echo "    fdt /ti/$DTB_NAME.dtb"
		echo "    append $append"
		echo "label tdm8-j3"
		echo "    menu label Linux $rel + TDM8 8-in on McASP0 (40-pin header J3)"
		echo "    kernel /Image-$rel.gz"
		echo "    fdtdir /"
		echo "    fdt /ti/$DTB_J3_NAME.dtb"
		echo "    append $append"
		# same kernel, stock device tree: isolates a TDM8 DT problem from a
		# kernel problem without reflashing
		echo "label notdm8"
		echo "    menu label Linux $rel, stock SK device tree (TDM8 off)"
		echo "    kernel /Image-$rel.gz"
		echo "    fdtdir /"
		echo "    fdt /ti/$DTB_STOCK.dtb"
		echo "    append $append"
		[ -n "$existing" ] && echo "$existing"
	} > "$BOOTDIR/extlinux/extlinux.conf.new"
	mv "$BOOTDIR/extlinux/extlinux.conf.new" "$BOOTDIR/extlinux/extlinux.conf"

	echo "staged into $BOOTDIR:"
	echo "  Image-$rel.gz"
	echo "  ti/$DTB_NAME.dtb  ti/$DTB_J3_NAME.dtb  ti/$DTB_STOCK.dtb"
	echo "  extlinux/extlinux.conf  (default=tdm8; kept: $(echo "$existing" | grep -c '^label ') pre-existing label(s))"
	echo "next: BOOTSRC=res/spare-sd-sk/boot UBOUT=u-boot-official/out_sk \\"
	echo "      R5_NAME=tiboot3-am62x-hs-fs-evm.bin KIMG_NAME=Image-$rel.gz \\"
	echo "      ROOTTAR=../buildroot/output/images/rootfs.tar.gz \\"
	echo "      ./build-spare-sd.sh res/spare-sd-sk/sk-am62b-tdm8.img"
}

deploy() {
	local rel; rel=$(M -s kernelrelease)
	for d in "$DTB_NAME" "$DTB_J3_NAME" "$DTB_STOCK"; do
		[ -f "$OUTDIR/$d.dtb" ] || { echo "run '$0 dtb' first" >&2; exit 1; }
	done
	tar -C "$STAGE/lib/modules" -czf /tmp/kmods-"$rel".tgz "$rel"
	scp "$KSRC/arch/arm64/boot/Image.gz" "$BOARD":/tmp/Image-"$rel".gz
	scp /tmp/kmods-"$rel".tgz "$BOARD":/tmp/
	scp "$OUTDIR/$DTB_NAME.dtb" "$OUTDIR/$DTB_J3_NAME.dtb" "$OUTDIR/$DTB_STOCK.dtb" "$BOARD":/tmp/
	ssh "$BOARD" "rel='$rel' DTB='$DTB_NAME' DTB_J3='$DTB_J3_NAME' DTB_STOCK='$DTB_STOCK' sh -s" <<'REMOTE'
set -e
# the board's tar may be BusyBox (no -z); modules go to a versioned dir so kernels coexist
gzip -dc /tmp/kmods-"$rel".tgz | tar -C /lib/modules -xf -
depmod "$rel" 2>/dev/null || true
mkdir -p /mnt/bootp; mount /dev/mmcblk1p1 /mnt/bootp
mkdir -p /mnt/bootp/ti /mnt/bootp/extlinux
cp /tmp/Image-"$rel".gz /mnt/bootp/Image-"$rel".gz
for d in "$DTB" "$DTB_J3" "$DTB_STOCK"; do cp /tmp/"$d".dtb /mnt/bootp/ti/"$d".dtb; done
[ -f /mnt/bootp/extlinux/extlinux.conf.orig ] || \
	cp /mnt/bootp/extlinux/extlinux.conf /mnt/bootp/extlinux/extlinux.conf.orig 2>/dev/null || true
A=$(grep -m1 'append' /mnt/bootp/extlinux/extlinux.conf.orig 2>/dev/null | sed 's/^[[:space:]]*append //')
[ -n "$A" ] || A="console=ttyS2,115200n8 earlycon=ns16550a,mmio32,0x02800000 root=/dev/mmcblk1p2 ro rootfstype=ext4 rootwait net.ifnames=0"
cat > /mnt/bootp/extlinux/extlinux.conf <<EXL
menu title TI SK-AM62B-P1 microSD
timeout 30
prompt 1
default tdm8
label tdm8
    kernel /Image-$rel.gz
    fdtdir /
    fdt /ti/$DTB.dtb
    append $A
label tdm8-j3
    kernel /Image-$rel.gz
    fdtdir /
    fdt /ti/$DTB_J3.dtb
    append $A
label notdm8
    kernel /Image-$rel.gz
    fdtdir /
    fdt /ti/$DTB_STOCK.dtb
    append $A
EXL
sync; umount /mnt/bootp
echo "deployed $rel + $DTB.dtb / $DTB_J3.dtb / $DTB_STOCK.dtb"
echo "enable the bridge with: sed -i 's/^TDM8_ENABLE=no/TDM8_ENABLE=yes/' /etc/tdm8/tdm8.env"
echo "for the tdm8-j3 label also set: TDM8_DIRECTION=capture"
echo "then reboot; the serial console can still pick 'notdm8'"
REMOTE
}

case "${1:-all}" in
fetch)  fetch ;;
shim)   fetch; shim ;;
dts)    fetch; dts ;;
config) fetch; shim; dts; config ;;
dtb)    dtb ;;
build)  build ;;
stage)  stage ;;
deploy) deploy ;;
all)    fetch; shim; dts; config; dtb; build; deploy ;;
image)  fetch; shim; dts; config; dtb; build; stage ;;
*) echo "Usage: $0 {fetch|shim|dts|config|dtb|build|stage|deploy|all|image}"; exit 1 ;;
esac
