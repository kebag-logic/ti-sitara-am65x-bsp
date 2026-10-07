#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: MIT

# Drop the PocketBeagle 2 device trees - the two TDM8 trees and the Kebag-Logic
# Ethernet Cap tree - into a kernel source tree and register them in
# arch/arm64/boot/dts/ti/Makefile.  Idempotent: safe to re-run
# after `build-tdm8-uac2-pb2.sh fetch` re-clones ./linux, and after a kernel bump.
#
# Like the SK trees and unlike the MYIR one, these are ordinary in-tree sources:
# mainline carries k3-am62-pocketbeagle2.dts, so they just #include it.
#
# Usage: apply-tdm8-pb2-dts.sh [<linux-src>]   (default: ../../linux)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
KSRC=${1:-$(cd "$HERE/../.." && pwd)/linux}
TI="$KSRC/arch/arm64/boot/dts/ti"

DTS_LIST="k3-am62-pocketbeagle2-tdm8 k3-am62-pocketbeagle2-tdm8-async k3-am62-pocketbeagle2-ethcap"

[ -f "$KSRC/Makefile" ] || { echo "not a kernel tree: $KSRC" >&2; exit 1; }
[ -f "$TI/k3-am62-pocketbeagle2.dts" ] || {
	echo "$TI/k3-am62-pocketbeagle2.dts missing - every tree here #includes it." >&2
	echo "It is present in v7.1, the tag this BSP pins; bump KVER if this tree is older." >&2
	exit 1
}

for d in $DTS_LIST; do
	cp "$HERE/$d.dts" "$TI/$d.dts"
done

# Register next to k3-am62-pocketbeagle2.dtb so `make dtbs` builds them too.  A
# direct `make ti/<name>.dtb` works without this, but only the Makefile entry
# makes them part of a full dtbs build and of `make dtbs_install`.
for d in $DTS_LIST; do
	if grep -q "^dtb-\$(CONFIG_ARCH_K3) += $d.dtb$" "$TI/Makefile"; then
		echo "Makefile: $d.dtb already registered"
		continue
	fi
	awk -v line="dtb-\$(CONFIG_ARCH_K3) += $d.dtb" '
	{ print }
	/^dtb-\$\(CONFIG_ARCH_K3\) \+= k3-am62-pocketbeagle2\.dtb$/ { print line }
	' "$TI/Makefile" > "$TI/Makefile.tdm8" && mv "$TI/Makefile.tdm8" "$TI/Makefile"
	grep -q "^dtb-\$(CONFIG_ARCH_K3) += $d.dtb$" "$TI/Makefile" || {
		echo "Makefile: could not anchor on k3-am62-pocketbeagle2.dtb" >&2; exit 1; }
	echo "Makefile: added $d.dtb"
done

echo "PocketBeagle 2 device trees applied to $KSRC"
