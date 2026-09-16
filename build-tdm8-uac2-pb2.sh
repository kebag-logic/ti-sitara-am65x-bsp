#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: MIT

# Build + deploy the TDM8 -> USB Audio Class 2.0 gadget stack for the
# BeagleBoard.org PocketBeagle 2 (AM6254, quad Cortex-A53).  Third sibling of
# build-tdm8-uac2.sh (MYIR AM6254) and build-tdm8-uac2-sk.sh (TI SK-AM62B-P1);
# all three share the codec shim (res/tdm8/kl-tdm8-dummy.c), the kernel
# fragment (res/kl-tdm8-uac2.config), the gadget builder and the alsaloop
# bridge.  Only the device tree and the board fragment differ.
#
# What is different about this board
# ----------------------------------
# PocketBeagle 2 brings a whole McASP0 out to the pre-soldered P1/P2 headers,
# so the 8x8 duplex link is four jumper wires and needs no soldering and no
# sacrificed peripheral - unlike the SK, where duplex means soldering to the
# OSPI flash pads.  Two trees are built:
#
#   k3-am62-pocketbeagle2-tdm8.dtb        McASP0, 8 in + 8 out, 4 wires (default)
#   k3-am62-pocketbeagle2-tdm8-async.dtb  same, separate RX clock pair, 6 wires
#
# and the stock k3-am62-pocketbeagle2.dtb is kept as a third extlinux entry, so
# a bad TDM8 tree is one serial-console keystroke away from a working boot.
# See README.tdm8-pb2.md.
#
# This script does not BUILD a bootloader - `./build.sh PB2` does that, from
# BeagleBoard's U-Boot fork (fetch.sh clones openbeagle.org/beagleboard/u-boot
# branch v2025.04-rc4-pocketbeagle2 into u-boot-pb/) into u-boot-pb/out_bp2/.
# It does VERIFY one: `$0 bootloader` checks that what came out is the HS-FS
# chain and that the real TI firmware is inside it.  PocketBeagle 2 is an HS-FS
# board - its binman description builds only hs and hs-fs images, no GP one at
# all - so the chain is:
#
#   tiboot3.bin  <- r5/tiboot3-am62x-hs-fs-evm.bin   (binman symlinks tiboot3.bin here)
#   tispl.bin    <- a53/tispl.bin                    (signed, NOT tispl.bin_unsigned)
#   u-boot.img   <- a53/u-boot.img                   (signed, NOT u-boot.img_unsigned)
#
# HS-FS is authenticated but not encrypted and not tied to a customer key: the
# signing key is U-Boot's own arch/arm/mach-k3/keys/custMpk.pem, so no
# TI_SECURE_DEV_PKG and no eFuse programming are involved.  See README.tdm8-pb2.md
# §5.  Everything else this script touches is the kernel, the device trees and
# extlinux.conf, which is all a TDM8 link needs.
#
# Usage: build-tdm8-uac2-pb2.sh [fetch|shim|dts|config|dtb|build|stage|deploy|probe|bootloader|all|image]
#        all        = ... build + deploy over ssh (live board)
#        image      = ... build + stage into res/spare-sd-pb2/boot for build-spare-sd.sh
#        probe      = ask a live board what it is (rev A1/AM6254 vs rev A0/AM6232)
#        bootloader = verify u-boot-pb/out_bp2 is a complete HS-FS chain
# Env:   KVER BOARD JOBS CROSS_COMPILE TDM8_DEFAULT_LABEL UBOUT_PB2 UB_VARIANT
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
KVER="${KVER:-v7.1}"
BOARD="${BOARD:-pb2}"              # ssh alias of a live PocketBeagle 2
JOBS="${JOBS:-$(nproc)}"
KSRC="$HERE/linux"
DTB_NAME="k3-am62-pocketbeagle2-tdm8"             # McASP0 8x8, 4 wires on P1/P2
DTB_ASYNC_NAME="k3-am62-pocketbeagle2-tdm8-async" # same + separate RX clocks
DTB_STOCK="k3-am62-pocketbeagle2"                 # untouched mainline tree, the fallback
OUTDIR="$HERE/res/tdm8-pb2"
STAGE="$HERE/.kstage-tdm8-pb2"
BOOTDIR="$HERE/res/spare-sd-pb2/boot"
# Where ./build.sh PB2 leaves the bootloaders, and the security variant this
# board is expected to be.  VARIANT=hs-fs => signed with U-Boot's in-tree demo
# key, no encryption of our payloads, no customer keys fused.
UBOUT_PB2="${UBOUT_PB2:-$HERE/u-boot-pb/out_bp2}"
UB_VARIANT="${UB_VARIANT:-hs-fs}"
# Which label the boot menu selects on its own.  "tdm8" is the four-wire
# synchronous tree; set TDM8_DEFAULT_LABEL=tdm8-async on a board whose FPGA
# drives two clock pairs.  Either way the other labels stay in the menu, so
# this only changes what happens when nobody touches the console.
TDM8_DEFAULT_LABEL="${TDM8_DEFAULT_LABEL:-tdm8}"
# console = main_uart6 (the 3-pin JST-SH debug port); k3-am62-pocketbeagle2.dts
# has stdout-path = &main_uart6 and aliases serial2 = &main_uart6, so it is
# ttyS2, and k3-am62-main.dtsi puts it at 0x02860000.  sdhci1 is the only MMC
# host the board enables (no eMMC), so the microSD is mmcblk0.
DEFAULT_APPEND="console=ttyS2,115200n8 earlycon=ns16550a,mmio32,0x02860000 root=/dev/mmcblk0p2 ro rootfstype=ext4 rootwait net.ifnames=0"
CROSS="${CROSS_COMPILE:-aarch64-linux-gnu-}"
# LOCALVERSION= (set but empty) stops setlocalversion appending "+" for an
# out-of-tag tree, so the release is exactly 7.1.0-tdm8-pb2 on every rebuild.
M() { make -C "$KSRC" ARCH=arm64 CROSS_COMPILE="$CROSS" LOCALVERSION= "$@"; }

fetch() {
	[ -f "$KSRC/Makefile" ] || git clone --depth 1 --branch "$KVER" \
		https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git "$KSRC"
}

# The codec shim is board-independent - same file, same Kconfig/Makefile hooks
# as the MYIR and SK builds.  Idempotent, so running all three is fine.
shim() { "$HERE/res/tdm8/apply-tdm8-kernel.sh" "$KSRC"; }

# Copy the two PocketBeagle 2 device trees in and register them in ti/Makefile.
dts() { "$HERE/res/tdm8-pb2/apply-tdm8-pb2-dts.sh" "$KSRC"; }

config() {
	# A live board's own config if there is one, otherwise plain arm64
	# defconfig - which already carries the whole K3 base.
	if ssh -o BatchMode=yes "$BOARD" 'zcat /proc/config.gz' > "$KSRC/.config" 2>/dev/null \
	   && [ -s "$KSRC/.config" ]; then
		echo "config: pulled /proc/config.gz from $BOARD"
	else
		echo "config: $BOARD unreachable, starting from arm64 defconfig" >&2
		rm -f "$KSRC/.config"
		M defconfig
	fi
	( cd "$KSRC" && ARCH=arm64 ./scripts/kconfig/merge_config.sh -m -O . \
		.config "$HERE/res/kl-tdm8-uac2.config" "$HERE/res/kl-pb2-am62.config" )
	M olddefconfig
	# olddefconfig only rewrites .config; setlocalversion (and so
	# `make kernelrelease`) reads include/config/auto.conf, which stays at the
	# previous board's value until a build syncs it.  All three TDM8 scripts
	# share ./linux, so refresh it here or `stage`/`deploy` name things wrongly.
	M syncconfig
	echo "kernelrelease=$(M -s kernelrelease)"
	for s in CONFIG_SND_SOC_KL_TDM8_DUMMY CONFIG_SND_SIMPLE_CARD \
	         CONFIG_SND_SOC_DAVINCI_MCASP CONFIG_USB_F_UAC2 CONFIG_USB_CONFIGFS \
	         CONFIG_USB_DWC3_AM62; do
		grep -q "^$s=[ym]" "$KSRC/.config" || { echo "$s did not survive olddefconfig" >&2; exit 1; }
	done
	# these decide whether the board can reach its own rootfs at all
	for s in CONFIG_MMC_SDHCI_AM654 CONFIG_REGULATOR_GPIO CONFIG_MFD_TPS65219 \
	         CONFIG_GPIO_DAVINCI; do
		grep -q "^$s=y" "$KSRC/.config" || { echo "$s must be =y (microSD power path)" >&2; exit 1; }
	done
}

# Build both TDM8 trees plus the stock tree used by the fallback entry.
dtb() {
	[ -f "$KSRC/.config" ] || { echo "run '$0 config' first" >&2; exit 1; }
	[ -f "$KSRC/arch/arm64/boot/dts/ti/$DTB_NAME.dts" ] || { echo "run '$0 dts' first" >&2; exit 1; }
	M "ti/$DTB_NAME.dtb" "ti/$DTB_ASYNC_NAME.dtb" "ti/$DTB_STOCK.dtb"
	mkdir -p "$OUTDIR"
	for d in "$DTB_NAME" "$DTB_ASYNC_NAME" "$DTB_STOCK"; do
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

# Stage the boot artifacts into res/spare-sd-pb2/boot/ so build-spare-sd.sh can
# make a card out of them.  Non-destructive: anything already staged there is
# kept as a fallback entry, and re-running replaces our own labels rather than
# stacking duplicates.
stage() {
	local rel append existing
	rel=$(M -s kernelrelease)
	for d in "$DTB_NAME" "$DTB_ASYNC_NAME" "$DTB_STOCK"; do
		[ -f "$OUTDIR/$d.dtb" ] || { echo "run '$0 dtb' first" >&2; exit 1; }
	done
	[ -f "$KSRC/arch/arm64/boot/Image.gz" ] || { echo "run '$0 build' first" >&2; exit 1; }

	mkdir -p "$BOOTDIR/ti" "$BOOTDIR/extlinux"
	cp "$KSRC/arch/arm64/boot/Image.gz" "$BOOTDIR/Image-$rel.gz"
	for d in "$DTB_NAME" "$DTB_ASYNC_NAME" "$DTB_STOCK"; do
		cp "$OUTDIR/$d.dtb" "$BOOTDIR/ti/$d.dtb"
	done

	# reuse whatever cmdline is already staged; otherwise the PocketBeagle 2
	# default derived above.
	# NB: grep -m1 stops per FILE, so two files yield two lines - head -1 after.
	append=$(grep -h 'append' "$BOOTDIR/extlinux/extlinux.conf" \
		"$BOOTDIR/extlinux/extlinux.conf.orig" 2>/dev/null |
		head -1 | sed 's/^[[:space:]]*append //')
	[ -n "$append" ] || append="$DEFAULT_APPEND"

	# everything from the first "label" onward, minus labels we own
	existing=$(awk '/^label /{ keep = ($2 != "tdm8" && $2 != "tdm8-async" && $2 != "notdm8") } keep' \
		"$BOOTDIR/extlinux/extlinux.conf" 2>/dev/null || true)

	{
		echo "menu title PocketBeagle 2 microSD (TDM8 -> UAC2)"
		echo "timeout 30"
		echo "prompt 1"
		echo "default $TDM8_DEFAULT_LABEL"
		echo "label tdm8"
		echo "    menu label Linux $rel + TDM8 8x8 on McASP0 (P2.01/P1.04/P1.02/P2.03)"
		echo "    kernel /Image-$rel.gz"
		echo "    fdtdir /"
		echo "    fdt /ti/$DTB_NAME.dtb"
		echo "    append $append"
		echo "label tdm8-async"
		echo "    menu label Linux $rel + TDM8 8x8 on McASP0, separate RX clocks (+P1.08/P1.06, UART1 off)"
		echo "    kernel /Image-$rel.gz"
		echo "    fdtdir /"
		echo "    fdt /ti/$DTB_ASYNC_NAME.dtb"
		echo "    append $append"
		# same kernel, stock device tree: isolates a TDM8 DT problem from a
		# kernel problem without reflashing
		echo "label notdm8"
		echo "    menu label Linux $rel, stock PocketBeagle 2 device tree (TDM8 off)"
		echo "    kernel /Image-$rel.gz"
		echo "    fdtdir /"
		echo "    fdt /ti/$DTB_STOCK.dtb"
		echo "    append $append"
		[ -n "$existing" ] && echo "$existing"
	} > "$BOOTDIR/extlinux/extlinux.conf.new"
	mv "$BOOTDIR/extlinux/extlinux.conf.new" "$BOOTDIR/extlinux/extlinux.conf"

	echo "staged into $BOOTDIR:"
	echo "  Image-$rel.gz"
	echo "  ti/$DTB_NAME.dtb  ti/$DTB_ASYNC_NAME.dtb  ti/$DTB_STOCK.dtb"
	echo "  extlinux/extlinux.conf  (default=$TDM8_DEFAULT_LABEL; kept: $(echo "$existing" | grep -c '^label ') pre-existing label(s))"
	echo
	echo "The bootloaders come from BeagleBoard's U-Boot fork, not u-boot-official."
	echo "PocketBeagle 2 is HS-FS: signed, not encrypted, no customer keys fused."
	echo "  ./build.sh PB2                 # -> u-boot-pb/out_bp2/{r5,a53}"
	echo "  $0 bootloader   # verify before flashing"
	echo "next: BOOTSRC=res/spare-sd-pb2/boot KIMG_NAME=Image-$rel.gz \\"
	echo "      UBOUT=u-boot-pb/out_bp2 \\"
	echo "      R5=u-boot-pb/out_bp2/r5/tiboot3-am62x-hs-fs-evm.bin \\"
	echo "      TISPL=u-boot-pb/out_bp2/a53/tispl.bin \\"
	echo "      UB=u-boot-pb/out_bp2/a53/u-boot.img \\"
	echo "      ROOTTAR=../buildroot/output/images/rootfs.tar.gz \\"
	echo "      ./build-spare-sd.sh res/spare-sd-pb2/pocketbeagle2-tdm8.img"
	echo "(or, to keep the card's own chain, skip build-spare-sd.sh entirely and"
	echo " use '$0 deploy', which touches only the kernel, DTBs and extlinux.conf)"
}

# Ask a live board what it is.  PocketBeagle 2 rev A1 (in production) carries
# the AM6254 - quad Cortex-A53; rev A0 (out of production) carried the dual-core
# AM6232.  Both run the same k3-am62-pocketbeagle2.dts, which #includes
# k3-am625.dtsi and so always describes four cores: on an A0 the two that are
# not fused in simply never come online.  So "4 cores" is a runtime fact, not a
# device-tree one, and this is how you check it.
probe() {
	ssh "$BOARD" 'sh -s' <<'REMOTE'
echo "model:   $(tr -d '\0' < /proc/device-tree/model 2>/dev/null)"
echo "cores:   $(grep -c ^processor /proc/cpuinfo) online, $(nproc --all 2>/dev/null || echo '?') possible"
echo "kernel:  $(uname -r)"
echo "memory:  $(awk '/MemTotal/{printf "%d MB\n", $2/1024}' /proc/meminfo)"
echo "mmc:     $(awk '$2=="/"{print $1}' /proc/mounts) is /"
if [ "$(grep -c ^processor /proc/cpuinfo)" -eq 4 ]; then
	echo "=> rev A1 / AM6254, quad Cortex-A53"
else
	echo "=> NOT four cores online - rev A0 / AM6232, or cores offline in sysfs"
fi
REMOTE
}

# Check, don't trust: binman runs with --allow-missing --fake-ext-blobs, so a
# bootloader built without BINMAN_INDIRS is a plausible-looking image with no TI
# firmware in it, and the ROM rejects that before the UART says anything.
bootloader() {
	[ -d "$UBOUT_PB2" ] || {
		echo "no $UBOUT_PB2 - build it first:" >&2
		echo "  ./fetch.sh          # clones u-boot-pb (BeagleBoard fork)" >&2
		echo "  ./build.sh PB2      # -> u-boot-pb/out_bp2/{r5,a53}" >&2
		exit 1
	}
	SOC=am62x VARIANT="$UB_VARIANT" "$HERE/res/uboot/check-bootloader.sh" "$UBOUT_PB2"
}

deploy() {
	local rel; rel=$(M -s kernelrelease)
	for d in "$DTB_NAME" "$DTB_ASYNC_NAME" "$DTB_STOCK"; do
		[ -f "$OUTDIR/$d.dtb" ] || { echo "run '$0 dtb' first" >&2; exit 1; }
	done
	[ -d "$STAGE/lib/modules/$rel" ] || { echo "run '$0 build' first" >&2; exit 1; }
	tar -C "$STAGE/lib/modules" -czf /tmp/kmods-"$rel".tgz "$rel"
	scp "$KSRC/arch/arm64/boot/Image.gz" "$BOARD":/tmp/Image-"$rel".gz
	scp /tmp/kmods-"$rel".tgz "$BOARD":/tmp/
	scp "$OUTDIR/$DTB_NAME.dtb" "$OUTDIR/$DTB_ASYNC_NAME.dtb" "$OUTDIR/$DTB_STOCK.dtb" "$BOARD":/tmp/
	ssh "$BOARD" "rel='$rel' DTB='$DTB_NAME' DTB_ASYNC='$DTB_ASYNC_NAME' DTB_STOCK='$DTB_STOCK' \
		DEFAULT_LABEL='$TDM8_DEFAULT_LABEL' DEFAULT_APPEND='$DEFAULT_APPEND' sh -s" <<'REMOTE'
set -e
# the board's tar may be BusyBox (no -z); modules go to a versioned dir so kernels coexist
gzip -dc /tmp/kmods-"$rel".tgz | tar -C /lib/modules -xf -
depmod "$rel" 2>/dev/null || true

# Find the FAT boot partition.  BeagleBoard's own images keep it mounted at
# /boot/firmware; a Buildroot card from build-spare-sd.sh does not mount it at
# all, in which case it is partition 1 of whatever disk carries /.
UMOUNT=
MNT=$(awk '$2=="/boot/firmware"{print $2}' /proc/mounts | head -1)
if [ -z "$MNT" ]; then
	ROOTDEV=$(awk '$2=="/"{print $1}' /proc/mounts | head -1)
	case "$ROOTDEV" in
	*p[0-9]) BOOTDEV="${ROOTDEV%p[0-9]}p1" ;;
	*[0-9])  BOOTDEV="${ROOTDEV%[0-9]}1" ;;
	*)       BOOTDEV=/dev/mmcblk0p1 ;;
	esac
	[ -b "$BOOTDEV" ] || { echo "no FAT boot partition found (tried $BOOTDEV)" >&2; exit 1; }
	MNT=/mnt/bootp; mkdir -p "$MNT"; mount "$BOOTDEV" "$MNT"; UMOUNT=1
fi
echo "boot partition: $MNT"

mkdir -p "$MNT/ti" "$MNT/extlinux"
cp /tmp/Image-"$rel".gz "$MNT/Image-$rel.gz"
for d in "$DTB" "$DTB_ASYNC" "$DTB_STOCK"; do cp /tmp/"$d".dtb "$MNT/ti/$d.dtb"; done

# keep one pristine copy of whatever shipped on the card, and inherit its
# cmdline so a board with a different root= keeps booting
[ -f "$MNT/extlinux/extlinux.conf.orig" ] || \
	cp "$MNT/extlinux/extlinux.conf" "$MNT/extlinux/extlinux.conf.orig" 2>/dev/null || true
A=$(grep -m1 'append' "$MNT/extlinux/extlinux.conf.orig" 2>/dev/null | sed 's/^[[:space:]]*append //')
[ -n "$A" ] || A="$DEFAULT_APPEND"

cat > "$MNT/extlinux/extlinux.conf" <<EXL
menu title PocketBeagle 2 microSD (TDM8 -> UAC2)
timeout 30
prompt 1
default $DEFAULT_LABEL
label tdm8
    menu label Linux $rel + TDM8 8x8 on McASP0 (P2.01/P1.04/P1.02/P2.03)
    kernel /Image-$rel.gz
    fdtdir /
    fdt /ti/$DTB.dtb
    append $A
label tdm8-async
    menu label Linux $rel + TDM8 8x8 on McASP0, separate RX clocks (+P1.08/P1.06)
    kernel /Image-$rel.gz
    fdtdir /
    fdt /ti/$DTB_ASYNC.dtb
    append $A
label notdm8
    menu label Linux $rel, stock PocketBeagle 2 device tree (TDM8 off)
    kernel /Image-$rel.gz
    fdtdir /
    fdt /ti/$DTB_STOCK.dtb
    append $A
EXL
sync
[ -n "$UMOUNT" ] && umount "$MNT"
echo "deployed $rel + $DTB.dtb / $DTB_ASYNC.dtb / $DTB_STOCK.dtb"
echo "cmdline in use: $A"
echo "enable the bridge with: sed -i 's/^TDM8_ENABLE=no/TDM8_ENABLE=yes/' /etc/tdm8/tdm8.env"
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
probe)  probe ;;
bootloader) bootloader ;;
all)    fetch; shim; dts; config; dtb; build; deploy ;;
image)  fetch; shim; dts; config; dtb; build; stage ;;
*) echo "Usage: $0 {fetch|shim|dts|config|dtb|build|stage|deploy|probe|bootloader|all|image}"; exit 1 ;;
esac
