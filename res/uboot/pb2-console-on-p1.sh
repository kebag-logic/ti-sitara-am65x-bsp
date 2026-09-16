#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: MIT

# Put the PocketBeagle 2 A53 U-Boot console on the P1 header instead of the
# JST-SH connector, so the WHOLE boot lands on one wire.
#
# Out of the box the boot log is split across two ports, which makes bring-up
# unnecessarily hard:
#
#   R5 SPL -> TF-A -> OP-TEE     main_uart0   P1.30 (TXD) / P1.32 (RXD)
#   A53 SPL -> U-Boot -> Linux   main_uart6   JST-SH 3-pin
#
# The handoff is the "Entry point address = 0x80080000" line: everything after
# it goes to the other connector.  If you only have a 3.3 V adapter on the
# header, the boot looks like it dies there.
#
# This points the A53 stages' stdout-path at main_uart0 as well.  main_uart0
# and main_uart0_pins_default both already carry bootph-all, so it survives
# fdtgrep into the SPL device tree - unlike main_uart6_pins_default, which does
# not (see fix-pb2-uart6-bootph.sh).
#
# Linux needs telling separately, because its console comes from the kernel
# command line, not from stdout-path.  main_uart0 is serial3 in the board
# aliases, so it is ttyS3:
#
#   console=ttyS3,115200n8 console=ttyS2,115200n8
#
# Listing both makes the kernel print to both; the LAST one is what /dev/console
# and the getty use, so keeping ttyS2 last leaves the login prompt where the
# Buildroot config expects it.
#
# Rebuild the A53 half afterwards, and put BOTH tispl.bin (which contains the
# A53 SPL) and u-boot.img back on the card.
#
# Idempotent.  Revert with:  fix-pb2-uart6-bootph.sh ... ; this script off
# Usage: pb2-console-on-p1.sh [on|off] [<u-boot-src>]   (default: on, ../../u-boot-pb)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
MODE=${1:-on}
UB=${2:-$(cd "$HERE/../.." && pwd)/u-boot-pb}
D="$UB/arch/arm/dts/k3-am6232-pocketbeagle2-u-boot.dtsi"
MARKER='KL: A53 console on P1'

[ -f "$UB/Makefile" ] || { echo "not a U-Boot tree: $UB" >&2; exit 1; }
[ -f "$D" ] || { echo "no $D - not a PocketBeagle 2 U-Boot tree" >&2; exit 1; }

case "$MODE" in
on)
	if grep -q "$MARKER" "$D"; then
		echo "console-on-P1: already applied"
		exit 0
	fi
	cat >> "$D" <<'DTS'

/* KL: A53 console on P1 - stdout-path here overrides the board .dts, which
 * points at main_uart6 (the JST-SH connector).  main_uart0 is P1.30/P1.32 and
 * is where the R5 SPL, TF-A and OP-TEE already print, so this puts the whole
 * boot on one wire.  Linux still needs console=ttyS3 on its command line. */
/ {
	chosen {
		stdout-path = &main_uart0;
	};
};
DTS
	echo "console-on-P1: applied to $D"
	;;
off)
	grep -q "$MARKER" "$D" || { echo "console-on-P1: not applied"; exit 0; }
	python3 - "$D" <<'PY'
import sys, re
p = sys.argv[1]
s = open(p).read()
i = s.index("\n/* KL: A53 console on P1")
j = s.index("};\n", s.index("stdout-path = &main_uart0;", i)) + 3
j = s.index("};\n", j) + 3          # close the chosen node, then the root node
open(p, "w").write(s[:i] + s[j:])
PY
	echo "console-on-P1: removed from $D"
	;;
*) echo "Usage: $0 [on|off] [<u-boot-src>]" >&2; exit 2 ;;
esac

echo "rebuild the A53 half, then copy BOTH tispl.bin and u-boot.img to the card"
