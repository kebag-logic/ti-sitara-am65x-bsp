#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: MIT

# Give PocketBeagle 2's U-Boot a persistent environment, for RAUC A/B updates
# (issue #27).
#
# BeagleBoard's PocketBeagle 2 defconfig keeps the environment nowhere
# (CONFIG_ENV_IS_NOWHERE): every saveenv is lost at the next reset. RAUC's
# U-Boot backend and the A/B bootchooser (res/ab/pb2-boot.cmd) keep their state
# there: BOOT_ORDER and the attempts left per slot, BOOT_A_LEFT and
# BOOT_B_LEFT. So the A53 defconfig gets the MYIR board's layout
# (README.safe-update.md, P0):
#
#   a redundant raw environment on the microSD (mmc 1, user area), in the gap
#   before the first partition, which starts at 1 MiB:
#
#     0x80000  primary    0x1f000 bytes
#     0xC0000  redundant  0x1f000 bytes
#
# /etc/fw_env.config on the PB2 image names the same two copies, for
# fw_printenv/fw_setenv. A card without an A/B layout boots as before: nothing
# writes the environment unless the bootchooser runs.
#
# The fork predates the Kconfig renames, so the names here are its own
# (SYS_REDUNDAND_ENVIRONMENT, SYS_MMC_ENV_DEV, SYS_MMC_ENV_PART).
#
# u-boot.img changes, and so does tispl.bin: its A53 SPL reads the environment
# too (CONFIG_SPL_ENV_SUPPORT comes with the defconfig's USB DFU fragment).
# tiboot3.bin is untouched.
#
# Idempotent. Usage: pb2-ab-env.sh {on|off} [<u-boot-src>]  (default: on, ../../u-boot-pb)
set -e

MODE=${1:-on}
HERE=$(cd "$(dirname "$0")" && pwd)
UB=${2:-$(cd "$HERE/../.." && pwd)/u-boot-pb}
D="$UB/configs/am6232_pocketbeagle2_a53_defconfig"
MARK="# --- kebag-logic: A/B environment for RAUC (res/uboot/pb2-ab-env.sh) ---"

[ -f "$UB/Makefile" ] || { echo "not a U-Boot tree: $UB" >&2; exit 1; }
[ -f "$D" ] || { echo "no $D - not a PocketBeagle 2 U-Boot tree" >&2; exit 1; }

present() {
	grep -qF "$MARK" "$D"
}

case "$MODE" in
on)
	if present; then
		echo "ab-env: already in $(basename "$D")"
		exit 0
	fi

	cat >> "$D" <<CONF
$MARK
# CONFIG_ENV_IS_NOWHERE is not set
CONFIG_ENV_IS_IN_MMC=y
CONFIG_SYS_REDUNDAND_ENVIRONMENT=y
CONFIG_ENV_SIZE=0x1f000
CONFIG_ENV_OFFSET=0x80000
CONFIG_ENV_OFFSET_REDUND=0xC0000
CONFIG_SYS_MMC_ENV_DEV=1
CONFIG_SYS_MMC_ENV_PART=0
CONF
	echo "ab-env: redundant environment on mmc 1 at 0x80000/0xC0000 added to $(basename "$D")"
	;;
off)
	if ! present; then
		echo "ab-env: not in $(basename "$D")"
		exit 0
	fi

	# the block runs from the marker to the end of the file
	sed -i "/^$(printf '%s' "$MARK" | sed 's/[][\/.*^$]/\\&/g')\$/,\$d" "$D"
	echo "ab-env: removed from $(basename "$D")"
	;;
*)
	echo "usage: $0 {on|off} [<u-boot-src>]" >&2
	exit 2
	;;
esac
