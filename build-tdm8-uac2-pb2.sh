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
# A fourth tree is for the Kebag-Logic PocketBeagle 2 Ethernet Cap (rev B):
#
#   k3-am62-pocketbeagle2-ethcap.dtb      DP83867IR on CPSW3G port 2 -> eth0
#
# The cap's RGMII2/MDIO lines sit on P1.02/P1.04/P2.01/P2.03, the TDM8 pins, so
# a board runs either the cap or the TDM8 link, never both.  All four trees go
# on every card; PB2_DEFAULT_LABEL=ethcap makes the cap's the one that boots.
# See README.pb2-ethcap.md.
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
#        sdimage    = build the microSD image (wraps build-spare-sd.sh; needs sudo)
#        abcard     = build the RAUC A/B microSD image, flashed once (no root needed)
#        bundle     = build a signed RAUC bundle for an A/B card (no root needed)
# Env:   KVER BOARD JOBS CROSS_COMPILE PB2_DEFAULT_LABEL PB2_ETHCAP_APPEND UBOUT_PB2 UB_VARIANT
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
KVER="${KVER:-v7.1}"
BOARD="${BOARD:-pb2}"              # ssh alias of a live PocketBeagle 2
JOBS="${JOBS:-$(nproc)}"
KSRC="$HERE/linux"
DTB_NAME="k3-am62-pocketbeagle2-tdm8"             # McASP0 8x8, 4 wires on P1/P2
DTB_ASYNC_NAME="k3-am62-pocketbeagle2-tdm8-async" # same + separate RX clocks
DTB_STOCK="k3-am62-pocketbeagle2"                 # untouched mainline tree, the fallback
DTB_ETHCAP_NAME="k3-am62-pocketbeagle2-ethcap"    # Kebag-Logic Ethernet Cap rev B, eth0
OUTDIR="$HERE/res/tdm8-pb2"
STAGE="$HERE/.kstage-tdm8-pb2"
BOOTDIR="$HERE/res/spare-sd-pb2/boot"
# Where ./build.sh PB2 leaves the bootloaders, and the security variant this
# board is expected to be.  VARIANT=hs-fs => signed with U-Boot's in-tree demo
# key, no encryption of our payloads, no customer keys fused.
UBOUT_PB2="${UBOUT_PB2:-$HERE/u-boot-pb/out_bp2}"
UB_VARIANT="${UB_VARIANT:-hs-fs}"
# Which label the boot menu selects on its own.  "tdm8" is the four-wire
# synchronous tree; "tdm8-async" is for an FPGA that drives two clock pairs;
# "ethcap" is for a board wearing the Ethernet Cap.  Every label stays in the
# menu, so this only changes what happens when nobody touches the console.
# TDM8_DEFAULT_LABEL is the old name and still works.
PB2_DEFAULT_LABEL="${PB2_DEFAULT_LABEL:-${TDM8_DEFAULT_LABEL:-tdm8}}"
case "$PB2_DEFAULT_LABEL" in
tdm8|tdm8-async|notdm8|ethcap) ;;
*) echo "PB2_DEFAULT_LABEL=$PB2_DEFAULT_LABEL: not one of tdm8 tdm8-async notdm8 ethcap" >&2; exit 1 ;;
esac
# Console.  PocketBeagle 2 splits its boot log across two UARTs out of the box,
# which makes bring-up painful:
#
#   main_uart0  ttyS3  0x02800000  P1.30 TXD / P1.32 RXD   R5 SPL, TF-A, OP-TEE
#   main_uart6  ttyS2  0x02860000  JST-SH 3-pin            A53 U-Boot, Linux
#
# res/uboot/pb2-console-on-p1.sh moves the A53 U-Boot stages onto main_uart0;
# this puts the kernel there too, so the ENTIRE boot - ROM through login - is on
# one wire.
#
# Deliberately a SINGLE console=.  Listing ttyS2 as well made the kernel log to
# both ports, which sounds harmless but means /dev/console fans out to two
# devices and the preferred console owning its input side depends on
# registration order.  One console, one owner, no ambiguity.  To put it on the
# JST-SH instead use console=ttyS2,115200n8 with earlycon base 0x02860000, and
# change BR2_TARGET_GENERIC_GETTY_PORT in the Buildroot defconfig to match.
#
# earlycon carries an explicit ,115200n8 so the earliest printks do not depend
# on whatever divisor U-Boot left in the register.  no-console-suspend keeps the
# console alive across a suspend; the port is not runtime-idled in either case,
# because 8250_omap sets an autosuspend delay of -1 for a node with no serdev
# children (8250_omap.c: "prevent an unsafe default policy with lossy characters
# on wake-up").
#
# root: sdhci1 is the only MMC host the board enables (there is no eMMC) - but
# it is NOT mmcblk0.  The board's aliases node says "mmc1 = &sdhci1", and
# mmc_alloc_host() takes host->index straight from of_alias_get_id(np, "mmc"),
# so the microSD comes up as mmcblk1 no matter that it is the only host.
# U-Boot agrees: the PB2 env sets mmcdev=1 / bootpart=1:2.
DEFAULT_APPEND="console=ttyS3,115200n8 earlycon=ns16550a,mmio32,0x02800000,115200n8 no-console-suspend root=/dev/mmcblk1p2 ro rootfstype=ext4 rootwait net.ifnames=0"
# Added to the "ethcap" label only.  CPU 3 is kept for the USB-to-Milan media
# plane: no scheduler tick (nohz_full), no RCU callbacks (rcu_nocbs), no
# unmanaged or managed IRQs, and out of the scheduler's load balancing.  The
# TDM8 labels keep all four cores.  Needs CONFIG_NO_HZ_FULL and
# CONFIG_RCU_NOCB_CPU (res/kl-pb2-am62.config).  Set it empty to boot the cap
# without isolation.
PB2_ETHCAP_APPEND="${PB2_ETHCAP_APPEND-isolcpus=nohz,domain,managed_irq,3 nohz_full=3 rcu_nocbs=3 irqaffinity=0-2}"
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
	# the USB-to-Milan bridge needs a real-time kernel and an isolatable core
	for s in CONFIG_PREEMPT_RT CONFIG_NO_HZ_FULL CONFIG_RCU_NOCB_CPU CONFIG_HZ_1000; do
		grep -q "^$s=y" "$KSRC/.config" || { echo "$s must be =y (real-time bridge)" >&2; exit 1; }
	done
	# the Ethernet Cap: eth0, its PHY, and the PTP clock / TSN offloads
	for s in CONFIG_TI_K3_AM65_CPSW_NUSS CONFIG_TI_DAVINCI_MDIO CONFIG_PHY_TI_GMII_SEL \
	         CONFIG_DP83867_PHY CONFIG_TI_K3_AM65_CPTS CONFIG_TI_AM65_CPSW_QOS; do
		grep -q "^$s=y" "$KSRC/.config" || { echo "$s must be =y (Ethernet Cap)" >&2; exit 1; }
	done
}

# Build both TDM8 trees, the Ethernet Cap tree, and the stock tree used by the
# fallback entry.
dtb() {
	[ -f "$KSRC/.config" ] || { echo "run '$0 config' first" >&2; exit 1; }
	for d in "$DTB_NAME" "$DTB_ETHCAP_NAME"; do
		[ -f "$KSRC/arch/arm64/boot/dts/ti/$d.dts" ] || { echo "run '$0 dts' first" >&2; exit 1; }
	done
	M "ti/$DTB_NAME.dtb" "ti/$DTB_ASYNC_NAME.dtb" "ti/$DTB_STOCK.dtb" "ti/$DTB_ETHCAP_NAME.dtb"
	mkdir -p "$OUTDIR"
	for d in "$DTB_NAME" "$DTB_ASYNC_NAME" "$DTB_STOCK" "$DTB_ETHCAP_NAME"; do
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
	for d in "$DTB_NAME" "$DTB_ASYNC_NAME" "$DTB_STOCK" "$DTB_ETHCAP_NAME"; do
		[ -f "$OUTDIR/$d.dtb" ] || { echo "run '$0 dtb' first" >&2; exit 1; }
	done
	[ -f "$KSRC/arch/arm64/boot/Image.gz" ] || { echo "run '$0 build' first" >&2; exit 1; }

	mkdir -p "$BOOTDIR/ti" "$BOOTDIR/extlinux"
	cp "$KSRC/arch/arm64/boot/Image.gz" "$BOOTDIR/Image-$rel.gz"
	for d in "$DTB_NAME" "$DTB_ASYNC_NAME" "$DTB_STOCK" "$DTB_ETHCAP_NAME"; do
		cp "$OUTDIR/$d.dtb" "$BOOTDIR/ti/$d.dtb"
	done

	# Inherit the cmdline ONLY from extlinux.conf.orig - a file that came off a
	# real card and is worth preserving.  Deliberately NOT from our own
	# extlinux.conf: inheriting from the previous run means a wrong default
	# survives every later fix, which is exactly how a stale root=/dev/mmcblk0p2
	# outlived the change that corrected it.
	append=$(grep -m1 'append' "$BOOTDIR/extlinux/extlinux.conf.orig" 2>/dev/null |
		sed 's/^[[:space:]]*append //')
	[ -n "$append" ] || append="$DEFAULT_APPEND"

	# everything from the first "label" onward, minus labels we own
	existing=$(awk '/^label /{ keep = ($2 != "tdm8" && $2 != "tdm8-async" && $2 != "notdm8" && $2 != "ethcap") } keep' \
		"$BOOTDIR/extlinux/extlinux.conf" 2>/dev/null || true)

	{
		echo "menu title PocketBeagle 2 microSD (TDM8 -> UAC2 / Ethernet Cap)"
		echo "timeout 30"
		echo "prompt 1"
		echo "default $PB2_DEFAULT_LABEL"
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
		# the cap's RGMII2/MDIO lines are the TDM8 pins: this one or a tdm8 one
		echo "label ethcap"
		echo "    menu label Linux $rel + Kebag-Logic Ethernet Cap rev B (DP83867 on RGMII2, eth0)"
		echo "    kernel /Image-$rel.gz"
		echo "    fdtdir /"
		echo "    fdt /ti/$DTB_ETHCAP_NAME.dtb"
		echo "    append $append${PB2_ETHCAP_APPEND:+ $PB2_ETHCAP_APPEND}"
		[ -n "$existing" ] && echo "$existing"
	} > "$BOOTDIR/extlinux/extlinux.conf.new"
	mv "$BOOTDIR/extlinux/extlinux.conf.new" "$BOOTDIR/extlinux/extlinux.conf"

	echo "staged into $BOOTDIR:"
	echo "  Image-$rel.gz"
	echo "  ti/$DTB_NAME.dtb  ti/$DTB_ASYNC_NAME.dtb  ti/$DTB_STOCK.dtb  ti/$DTB_ETHCAP_NAME.dtb"
	echo "  extlinux/extlinux.conf  (default=$PB2_DEFAULT_LABEL; kept: $(echo "$existing" | grep -c '^label ') pre-existing label(s))"
	echo
	echo "The bootloaders come from BeagleBoard's U-Boot fork, not u-boot-official."
	echo "PocketBeagle 2 is HS-FS: signed, not encrypted, no customer keys fused."
	echo "  ./build.sh PB2                 # -> u-boot-pb/out_bp2/{r5,a53}"
	echo "  $0 bootloader   # verify before flashing"
	echo "next, to build the card:"
	echo
	echo "    $0 sdimage"
	echo
	echo "which fills in BOOTSRC/UBOUT/R5/TISPL/UB/KIMG_NAME/ROOTTAR for this"
	echo "board and checks each one exists first. Do NOT hand-write that call to"
	echo "build-spare-sd.sh: every default in it points at the MYIR GP chain, so"
	echo "one dropped assignment gives you a card built from another board's"
	echo "bootloader, or a \"missing input: .../res/spare-sd/boot/...\" naming a"
	echo "path you never typed."
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

# Build the microSD image, with every PocketBeagle 2 path filled in.
#
# build-spare-sd.sh defaults BOOTSRC, UBOUT, R5, TISPL, UB, KIMG_NAME and
# ROOTTAR to the MYIR GP chain, and the *_unsigned twins live in the same
# directory as the signed ones this board needs.  Passing all seven by hand is
# a paste away from a card built out of another board's bootloader, or from a
# "missing input: .../res/spare-sd/boot/Image-....gz" when one assignment gets
# dropped.  So don't pass them by hand.
sdimage() {
	local rel out roottar r5
	rel=$(M -s kernelrelease)
	# named after the label the staged extlinux.conf boots: pocketbeagle2-tdm8.img,
	# pocketbeagle2-ethcap.img, ...  so a cap card and a TDM8 card never overwrite
	# each other
	local label
	label=$(awk '$1 == "default" { print $2; exit }' "$BOOTDIR/extlinux/extlinux.conf" 2>/dev/null || true)
	out="${1:-$HERE/res/spare-sd-pb2/pocketbeagle2-${label:-tdm8}.img}"
	roottar="${ROOTTAR:-$HERE/../buildroot-pb2/images/rootfs.tar.gz}"
	r5="$UBOUT_PB2/r5/tiboot3-am62x-$UB_VARIANT-evm.bin"

	# Fail with the name of what is actually missing, rather than letting
	# build-spare-sd.sh report a default path nobody asked for.
	local miss=0
	for f in "$BOOTDIR/Image-$rel.gz" "$BOOTDIR/extlinux/extlinux.conf" \
	         "$r5" "$UBOUT_PB2/a53/tispl.bin" "$UBOUT_PB2/a53/u-boot.img" \
	         "$roottar"; do
		[ -s "$f" ] || { echo "missing: $f" >&2; miss=1; }
	done
	if [ "$miss" -ne 0 ]; then
		echo >&2
		echo "  kernel + DTBs + extlinux : $0 image" >&2
		echo "  bootloaders              : ./build.sh PB2  (then $0 bootloader)" >&2
		echo "  rootfs                   : make -C ../buildroot O=\$PWD/../buildroot-pb2 BR2_JLEVEL=32" >&2
		echo "  or point ROOTTAR= at another rootfs.tar.gz" >&2
		exit 1
	fi

	echo "building $out"
	echo "  boot     $BOOTDIR (Image-$rel.gz, default label ${label:-?})"
	echo "  chain    $UB_VARIANT: $(basename "$r5"), tispl.bin, u-boot.img"
	echo "  rootfs   $roottar"
	BOOTSRC="$BOOTDIR" \
	UBOUT="$UBOUT_PB2" \
	R5="$r5" \
	TISPL="$UBOUT_PB2/a53/tispl.bin" \
	UB="$UBOUT_PB2/a53/u-boot.img" \
	KIMG_NAME="Image-$rel.gz" \
	ROOTTAR="$roottar" \
		"$HERE/build-spare-sd.sh" "$out"
}

deploy() {
	local rel; rel=$(M -s kernelrelease)
	for d in "$DTB_NAME" "$DTB_ASYNC_NAME" "$DTB_STOCK" "$DTB_ETHCAP_NAME"; do
		[ -f "$OUTDIR/$d.dtb" ] || { echo "run '$0 dtb' first" >&2; exit 1; }
	done
	[ -d "$STAGE/lib/modules/$rel" ] || { echo "run '$0 build' first" >&2; exit 1; }
	tar -C "$STAGE/lib/modules" -czf /tmp/kmods-"$rel".tgz "$rel"
	scp "$KSRC/arch/arm64/boot/Image.gz" "$BOARD":/tmp/Image-"$rel".gz
	scp /tmp/kmods-"$rel".tgz "$BOARD":/tmp/
	scp "$OUTDIR/$DTB_NAME.dtb" "$OUTDIR/$DTB_ASYNC_NAME.dtb" "$OUTDIR/$DTB_STOCK.dtb" \
	    "$OUTDIR/$DTB_ETHCAP_NAME.dtb" "$BOARD":/tmp/
	ssh "$BOARD" "rel='$rel' DTB='$DTB_NAME' DTB_ASYNC='$DTB_ASYNC_NAME' DTB_STOCK='$DTB_STOCK' \
		DTB_ETHCAP='$DTB_ETHCAP_NAME' ETHCAP_APPEND='$PB2_ETHCAP_APPEND' \
		DEFAULT_LABEL='$PB2_DEFAULT_LABEL' DEFAULT_APPEND='$DEFAULT_APPEND' sh -s" <<'REMOTE'
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
	*)       BOOTDEV=/dev/mmcblk1p1 ;;
	esac
	[ -b "$BOOTDEV" ] || { echo "no FAT boot partition found (tried $BOOTDEV)" >&2; exit 1; }
	MNT=/mnt/bootp; mkdir -p "$MNT"; mount "$BOOTDEV" "$MNT"; UMOUNT=1
fi
echo "boot partition: $MNT"

mkdir -p "$MNT/ti" "$MNT/extlinux"
cp /tmp/Image-"$rel".gz "$MNT/Image-$rel.gz"
for d in "$DTB" "$DTB_ASYNC" "$DTB_STOCK" "$DTB_ETHCAP"; do cp /tmp/"$d".dtb "$MNT/ti/$d.dtb"; done

# keep one pristine copy of whatever shipped on the card, and inherit its
# cmdline so a board with a different root= keeps booting
[ -f "$MNT/extlinux/extlinux.conf.orig" ] || \
	cp "$MNT/extlinux/extlinux.conf" "$MNT/extlinux/extlinux.conf.orig" 2>/dev/null || true
A=$(grep -m1 'append' "$MNT/extlinux/extlinux.conf.orig" 2>/dev/null | sed 's/^[[:space:]]*append //')
[ -n "$A" ] || A="$DEFAULT_APPEND"

cat > "$MNT/extlinux/extlinux.conf" <<EXL
menu title PocketBeagle 2 microSD (TDM8 -> UAC2 / Ethernet Cap)
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
label ethcap
    menu label Linux $rel + Kebag-Logic Ethernet Cap rev B (DP83867 on RGMII2, eth0)
    kernel /Image-$rel.gz
    fdtdir /
    fdt /ti/$DTB_ETHCAP.dtb
    append $A${ETHCAP_APPEND:+ $ETHCAP_APPEND}
EXL
sync
[ -n "$UMOUNT" ] && umount "$MNT"
echo "deployed $rel + $DTB.dtb / $DTB_ASYNC.dtb / $DTB_STOCK.dtb / $DTB_ETHCAP.dtb (default $DEFAULT_LABEL)"
echo "cmdline in use: $A"
echo "enable the bridge with: sed -i 's/^TDM8_ENABLE=no/TDM8_ENABLE=yes/' /etc/tdm8/tdm8.env"
echo "then reboot; the serial console can still pick 'notdm8'"
REMOTE
}

# ---- RAUC A/B (issue #27) --------------------------------------------------
#
# abcard [out.img]    an A/B microSD image, flashed once:
#                       p1  FAT  BOOT      tiboot3.bin, tispl.bin, u-boot.img,
#                                          boot.scr (the bootchooser,
#                                          res/ab/pb2-boot.cmd.in), and no
#                                          extlinux.conf
#                       p2  ext4 rootfs.A  a complete slot: the rootfs, /boot
#                       p3  ext4 rootfs.B  (Image.gz and the four device
#                                          trees), /lib/modules
#                       p4  ext4 data      /data, which survives slot switches
#                     Every update after that goes through RAUC. The
#                     bootloaders must have the A/B environment
#                     (res/uboot/pb2-ab-env.sh, on by default in ./build.sh PB2).
# bundle [out.raucb]  a signed RAUC bundle of one complete slot, compatible
#                     pocketbeagle2-am62x: rauc install <bundle> on the board.
#
# Both are built without root: fakeroot keeps the rootfs's owners, mke2fs -d
# fills the ext4 slots, mtools the FAT, sfdisk and dd the image file.

AB_BOOT_MB="${AB_BOOT_MB:-300}"
AB_SLOT_MB="${AB_SLOT_MB:-1536}"
AB_DATA_MB="${AB_DATA_MB:-256}"
RAUC_COMPATIBLE="pocketbeagle2-am62x"
RAUC_CERT="${RAUC_CERT:-$HERE/res/rauc/rauc-dev.cert.pem}"
RAUC_KEY="${RAUC_KEY:-$HERE/res/rauc/rauc-dev.key.pem}"
# host-rauc and mksquashfs, from a Buildroot output that built them
BR_HOST="${BR_HOST:-$HERE/../buildroot/output/host}"

# The "ethcap" label's kernel arguments, without root= and ro: the bootchooser
# adds the slot's own.
ab_append() {
	echo "$DEFAULT_APPEND${PB2_ETHCAP_APPEND:+ $PB2_ETHCAP_APPEND}" | sed 's/ root=[^ ]*//; s/ ro / /'
}

# Check what a slot is made of, and print the kernel release.
ab_inputs() {
	local rel roottar="$1" miss=0
	rel=$(M -s kernelrelease)

	for f in "$KSRC/arch/arm64/boot/Image.gz" "$roottar"; do
		[ -s "$f" ] || { echo "missing: $f" >&2; miss=1; }
	done

	for d in "$DTB_NAME" "$DTB_ASYNC_NAME" "$DTB_STOCK" "$DTB_ETHCAP_NAME"; do
		[ -s "$OUTDIR/$d.dtb" ] || { echo "missing: $OUTDIR/$d.dtb" >&2; miss=1; }
	done

	[ -d "$STAGE/lib/modules/$rel" ] || { echo "missing: $STAGE/lib/modules/$rel" >&2; miss=1; }

	if [ "$miss" -ne 0 ]; then
		echo "  kernel, device trees, modules: $0 build && $0 dtb" >&2
		echo "  rootfs: make -C ../buildroot O=\$PWD/../buildroot-pb2 (or ROOTTAR=)" >&2
		exit 1
	fi

	echo "$rel"
}

# Fill directory $1 with one complete slot (run under fakeroot, so the rootfs
# keeps its owners): the rootfs from $3, /boot, /lib/modules/$2.
ab_slot_tree() {
	local dir="$1" rel="$2" roottar="$3"

	tar -C "$dir" -xzf "$roottar"

	mkdir -p "$dir/boot" "$dir/lib/modules"
	cp "$KSRC/arch/arm64/boot/Image.gz" "$dir/boot/Image.gz"

	for d in "$DTB_NAME" "$DTB_ASYNC_NAME" "$DTB_STOCK" "$DTB_ETHCAP_NAME"; do
		cp "$OUTDIR/$d.dtb" "$dir/boot/$d.dtb"
	done

	rm -rf "$dir/lib/modules/$rel"
	cp -a "$STAGE/lib/modules/$rel" "$dir/lib/modules/"
}

abcard() {
	local out roottar rel r5 w mkimage
	roottar="${ROOTTAR:-$HERE/../buildroot-pb2/images/rootfs.tar.gz}"
	out="${1:-$HERE/res/spare-sd-pb2/pocketbeagle2-ethcap-ab.img}"
	r5="$UBOUT_PB2/r5/tiboot3-am62x-$UB_VARIANT-evm.bin"
	mkimage="$UBOUT_PB2/a53/tools/mkimage"

	rel=$(ab_inputs "$roottar")

	for f in "$r5" "$UBOUT_PB2/a53/tispl.bin" "$UBOUT_PB2/a53/u-boot.img" "$mkimage"; do
		[ -s "$f" ] || { echo "missing: $f (./build.sh PB2)" >&2; exit 1; }
	done

	# a bootloader without the A/B environment would forget every attempt
	grep -q '^CONFIG_ENV_IS_IN_MMC=y' "$UBOUT_PB2/a53/.config" || {
		echo "$UBOUT_PB2 keeps no environment: rebuild with PB2_AB_ENV=on ./build.sh PB2" >&2
		exit 1
	}

	w=$(mktemp -d)
	trap 'rm -rf "$w"' EXIT

	echo "building $out"
	echo "  slots    $rel, rootfs $roottar"
	echo "  sizes    boot $AB_BOOT_MB, rootfs.A/B $AB_SLOT_MB each, data $AB_DATA_MB MiB"

	# the bootchooser, with the ethcap label's arguments
	sed "s|@APPEND@|$(ab_append)|" "$HERE/res/ab/pb2-boot.cmd.in" > "$w/boot.cmd"
	"$mkimage" -A arm64 -T script -C none -n "PB2 A/B bootchooser" -d "$w/boot.cmd" "$w/boot.scr" >/dev/null

	# p1: the FAT, as build-spare-sd.sh makes it (mformat keeps the full
	# total-sector count the K3 ROM wants)
	truncate -s "${AB_BOOT_MB}M" "$w/boot.vfat"
	mformat -i "$w/boot.vfat" -R 6 -F -v BOOT ::
	mcopy -i "$w/boot.vfat" "$r5" ::/tiboot3.bin
	mcopy -i "$w/boot.vfat" "$UBOUT_PB2/a53/tispl.bin" ::/tispl.bin
	mcopy -i "$w/boot.vfat" "$UBOUT_PB2/a53/u-boot.img" ::/u-boot.img
	mcopy -i "$w/boot.vfat" "$w/boot.scr" ::/boot.scr

	# p2, p3: the same slot twice; p4: an empty data file system
	mkdir "$w/slot"
	export -f ab_slot_tree
	export KSRC OUTDIR STAGE CROSS DTB_NAME DTB_ASYNC_NAME DTB_STOCK DTB_ETHCAP_NAME
	fakeroot -- bash -c '
		set -e
		ab_slot_tree "$1/slot" "$2" "$3"
		mke2fs -q -t ext4 -L rootfs.A -d "$1/slot" "$1/rootfs.A.ext4" "${4}M"
		mke2fs -q -t ext4 -L rootfs.B -d "$1/slot" "$1/rootfs.B.ext4" "${4}M"
	' _ "$w" "$rel" "$roottar" "$AB_SLOT_MB"
	mke2fs -q -t ext4 -L data "$w/data.ext4" "${AB_DATA_MB}M"

	# the card: an MBR (the K3 ROM finds the FAT through it), partitions
	# aligned on 1 MiB, each written at its start
	rm -f "$out"
	truncate -s "$((1 + AB_BOOT_MB + 2 * AB_SLOT_MB + AB_DATA_MB + 1))M" "$out"
	sfdisk -q "$out" <<SF
label: dos
start=2048, size=$((AB_BOOT_MB * 2048)), type=c, bootable
size=$((AB_SLOT_MB * 2048)), type=83
size=$((AB_SLOT_MB * 2048)), type=83
size=$((AB_DATA_MB * 2048)), type=83
SF

	local n=1 start
	for part in boot.vfat rootfs.A.ext4 rootfs.B.ext4 data.ext4; do
		start=$(sfdisk -q --dump "$out" | awk -v n="$n" '$1 ~ ("img" n "$") { sub(",", "", $4); print $4 }')
		dd if="$w/$part" of="$out" bs=512 seek="$start" conv=notrunc,sparse status=none
		n=$((n + 1))
	done

	rm -rf "$w"
	trap - EXIT

	echo "built $out ($(du -h --apparent-size "$out" | cut -f1), $(du -h "$out" | cut -f1) on disk)"
	sfdisk -l "$out" | sed -n '/^Device/,$p'
	echo "flash once: sudo dd if=$out of=/dev/sdX bs=4M conv=fsync status=progress"
	echo "then update with: $0 bundle && rauc install <bundle> on the board"
}

bundle() {
	local out roottar rel w version
	roottar="${ROOTTAR:-$HERE/../buildroot-pb2/images/rootfs.tar.gz}"
	out="${1:-$HERE/res/rauc/pocketbeagle2-am62x.raucb}"
	version="${VERSION:-$(git -C "$HERE" describe --always --dirty)-$(date +%Y%m%d%H%M)}"

	rel=$(ab_inputs "$roottar")

	export PATH="$BR_HOST/bin:$PATH"
	command -v rauc >/dev/null || { echo "no rauc: make -C ../buildroot host-rauc (or BR_HOST=)" >&2; exit 1; }
	command -v mksquashfs >/dev/null || { echo "no mksquashfs in $BR_HOST/bin" >&2; exit 1; }

	for f in "$RAUC_CERT" "$RAUC_KEY"; do
		[ -s "$f" ] || { echo "missing: $f" >&2; exit 1; }
	done

	w=$(mktemp -d)
	trap 'rm -rf "$w"' EXIT

	echo "building $out ($RAUC_COMPATIBLE $version, $rel)"

	# one complete slot as a tar image: RAUC formats the inactive slot and
	# extracts it, so the slot's size is the partition's, not the bundle's
	mkdir "$w/slot" "$w/content"
	export -f ab_slot_tree
	export KSRC OUTDIR STAGE CROSS DTB_NAME DTB_ASYNC_NAME DTB_STOCK DTB_ETHCAP_NAME
	fakeroot -- bash -c '
		set -e
		ab_slot_tree "$1/slot" "$2" "$3"
		tar -C "$1/slot" --numeric-owner -czf "$1/content/rootfs.tar.gz" .
	' _ "$w" "$rel" "$roottar"

	cat > "$w/content/manifest.raucm" <<MF
[update]
compatible=$RAUC_COMPATIBLE
version=$version

[bundle]
format=plain

[image.rootfs]
filename=rootfs.tar.gz
MF

	mkdir -p "$(dirname "$out")"
	rm -f "$out"
	rauc bundle --cert="$RAUC_CERT" --key="$RAUC_KEY" "$w/content" "$out"
	rauc info --keyring="$RAUC_CERT" "$out" | sed -n '1,12p'

	rm -rf "$w"
	trap - EXIT

	echo "built $out ($(du -h "$out" | cut -f1))"
	echo "install: scp $out <board>:/tmp/ && ssh <board> rauc install /tmp/$(basename "$out")"
}

case "${1:-all}" in
fetch)  fetch ;;
shim)   fetch; shim ;;
dts)    fetch; dts ;;
config) fetch; shim; dts; config ;;
dtb)    dtb ;;
build)  shim; build ;;
stage)  stage ;;
deploy) deploy ;;
probe)  probe ;;
bootloader) bootloader ;;
sdimage) shift 2>/dev/null; sdimage "$@" ;;
abcard) shift 2>/dev/null; abcard "$@" ;;
bundle) shift 2>/dev/null; bundle "$@" ;;
all)    fetch; shim; dts; config; dtb; build; deploy ;;
image)  fetch; shim; dts; config; dtb; build; stage ;;
*) echo "Usage: $0 {fetch|shim|dts|config|dtb|build|stage|deploy|probe|bootloader|sdimage|abcard|bundle|all|image}"; exit 1 ;;
esac
