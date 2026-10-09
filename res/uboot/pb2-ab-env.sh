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
# Only U-Boot proper (u-boot.img) gets the environment. The A53 SPL keeps it
# nowhere, as it did before: it needs none, and the defconfig's USB DFU
# fragment would otherwise give it CONFIG_SPL_ENV_IS_IN_MMC too.
#
# U-Boot proper also gets the RTI watchdog driver and the `wdt` command, so
# the bootchooser can start RTI0 just before booti: a kernel that hangs
# before Linux takes the watchdog over resets the board, and costs its slot
# an attempt.
#
#   - WATCHDOG_AUTOSTART is off. It would start every RTI in the device tree
#     (one per A53 core, and the DM R5's), and Linux pets only RTI0; it
#     would also run while someone sits at the console.
#   - WATCHDOG (U-Boot petting what it started) is off: RTI0 is started as
#     the last step before booti, and Linux's rti_wdt takes it over within
#     seconds, well inside its 60 s. WATCHDOG would also change the A53
#     SPL (the hash functions then pause between chunks to pet), which
#     stays byte-identical to the stock build's this way.
#   - U-Boot's device tree keeps RTI0 only. With WDT, U-Boot probes every
#     watchdog at start-up, autostart or not, and probing an RTI powers it
#     on through TI SCI as exclusive (k3-am62-main.dtsi's power-domains),
#     a claim U-Boot never gives back before booti. RTI1-3 belong to A53
#     cores 1-3: with them claimed, TF-A could not start those cores, and
#     Linux came up on CPU0 alone ("CPU1: failed to come online"). Linux
#     has its own device tree, and still sees all five.
#
# Idempotent. Usage: pb2-ab-env.sh {on|off} [<u-boot-src>]  (default: on, ../../u-boot-pb)
set -e

MODE=${1:-on}
HERE=$(cd "$(dirname "$0")" && pwd)
UB=${2:-$(cd "$HERE/../.." && pwd)/u-boot-pb}
D="$UB/configs/am6232_pocketbeagle2_a53_defconfig"
MARK="# --- kebag-logic: A/B environment for RAUC (res/uboot/pb2-ab-env.sh) ---"
DT="$UB/arch/arm/dts/k3-am6232-pocketbeagle2-u-boot.dtsi"
DT_MARK="/* --- kebag-logic: U-Boot keeps RTI0 only (res/uboot/pb2-ab-env.sh) --- */"
DT_END="/* --- kebag-logic: end of RTI0 only --- */"

[ -f "$UB/Makefile" ] || { echo "not a U-Boot tree: $UB" >&2; exit 1; }
[ -f "$D" ] || { echo "no $D - not a PocketBeagle 2 U-Boot tree" >&2; exit 1; }
[ -f "$DT" ] || { echo "no $DT - not a PocketBeagle 2 U-Boot tree" >&2; exit 1; }

# a sed address for a line holding exactly <text>
line_re() {
	printf '%s' "$1" | sed 's/[][\/.*^$]/\\&/g'
}

dt_present() {
	grep -qF "$DT_MARK" "$DT"
}

dt_on() {
	dt_present && return 0

	cat >> "$DT" <<DTS
$DT_MARK
&main_rti1 {
	status = "disabled";
};

&main_rti2 {
	status = "disabled";
};

&main_rti3 {
	status = "disabled";
};

&main_rti15 {
	status = "disabled";
};
$DT_END
DTS
	echo "ab-env: U-Boot's device tree keeps RTI0 only ($(basename "$DT"))"
}

dt_off() {
	dt_present || return 0

	sed -i "/^$(line_re "$DT_MARK")\$/,/^$(line_re "$DT_END")\$/d" "$DT"
	echo "ab-env: RTI override removed from $(basename "$DT")"
}

present() {
	grep -qF "$MARK" "$D"
}

case "$MODE" in
on)
	dt_on

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
CONFIG_SPL_ENV_IS_NOWHERE=y
# CONFIG_SPL_ENV_IS_IN_MMC is not set
CONFIG_WDT=y
CONFIG_WDT_K3_RTI=y
CONFIG_CMD_WDT=y
# CONFIG_WATCHDOG is not set
# CONFIG_WATCHDOG_AUTOSTART is not set
# CONFIG_SPL_WDT is not set
CONF
	echo "ab-env: redundant environment on mmc 1 at 0x80000/0xC0000, and the RTI watchdog, added to $(basename "$D")"
	;;
off)
	dt_off

	if ! present; then
		echo "ab-env: not in $(basename "$D")"
		exit 0
	fi

	# the block runs from the marker to the end of the file
	sed -i "/^$(line_re "$MARK")\$/,\$d" "$D"
	echo "ab-env: removed from $(basename "$D")"
	;;
*)
	echo "usage: $0 {on|off} [<u-boot-src>]" >&2
	exit 2
	;;
esac
