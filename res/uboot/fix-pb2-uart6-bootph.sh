#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: MIT

# Give the PocketBeagle 2 A53 SPL a working console.
#
# The symptom: a perfectly healthy boot that goes silent forever at
#
#   INFO:    BL31: Preparing for EL3 exit to normal world
#   INFO:    Entry point address = 0x80080000
#
# with nothing on the JST-SH debug connector afterwards.
#
# The cause is a bootph asymmetry in the board device tree.  The A53 SPL's
# console is main_uart6 (chosen/stdout-path), and k3-am6232-pocketbeagle2.dts
# does give bootph-all to the &main_uart6 *node* - but NOT to the pinmux group
# main_uart6_pins_default that the node's pinctrl-0 points at.  fdtgrep keeps
# only nodes carrying a bootph-* property when it builds the SPL device tree,
# so the group is dropped and pinctrl-0 is left pointing at a phandle that no
# longer resolves:
#
#   spl/dts/ti/k3-am6232-pocketbeagle2.dtb:
#     stdout-path = "/bus@f0000/serial@2860000"     <- wants UART6
#     serial@2860000 { pinctrl-0 = <0x13>; }        <- dangling
#     main-uart6-default-pins                       <- 0 occurrences
#
# OSPI0_D4/D5 are therefore never muxed to UART6_RXD/TXD, and the SPL writes
# into a UART that is not connected to any pad.  main_uart0_pins_default, which
# the R5 SPL uses, does carry bootph-all - which is exactly why the earlier R5
# output arrives normally on P1.30 and then everything stops.
#
# U-Boot proper is unaffected: it uses the unstripped tree, where the group is
# present, so it muxes UART6 and prints.  Only the SPL stage is mute.
#
# The fix is one property, applied in the -u-boot.dtsi so the upstream-synced
# board .dts stays untouched.
#
# Rebuild the A53 half afterwards: the A53 SPL lives inside tispl.bin, so BOTH
# tispl.bin and u-boot.img have to go back on the card.
#
# Idempotent.  Usage: fix-pb2-uart6-bootph.sh [<u-boot-src>]  (default: ../../u-boot-pb)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
UB=${1:-$(cd "$HERE/../.." && pwd)/u-boot-pb}
D="$UB/arch/arm/dts/k3-am6232-pocketbeagle2-u-boot.dtsi"
MARKER='main_uart6_pins_default'

[ -f "$UB/Makefile" ] || { echo "not a U-Boot tree: $UB" >&2; exit 1; }
[ -f "$D" ] || { echo "no $D - not a PocketBeagle 2 U-Boot tree" >&2; exit 0; }

if grep -q "$MARKER" "$D"; then
	echo "uart6 bootph: already present in $(basename "$D")"
	exit 0
fi

cat >> "$D" <<'DTS'

/*
 * The A53 SPL's console is main_uart6, but upstream gives bootph-all to
 * &main_uart6 and not to the pinmux group its pinctrl-0 points at, so fdtgrep
 * drops the group from the SPL device tree and the SPL ends up writing into a
 * UART whose pads were never muxed.  The boot is silent from "Entry point
 * address = 0x80080000" until U-Boot proper, which uses the unstripped tree.
 * main_uart0_pins_default already carries bootph-all, which is why the R5 SPL
 * on P1.30 is unaffected.
 */
&main_uart6_pins_default {
	bootph-all;
};
DTS

echo "uart6 bootph: appended to $D"
echo "now rebuild the A53 half - the A53 SPL is inside tispl.bin, so tispl.bin"
echo "AND u-boot.img both have to go back on the card"
