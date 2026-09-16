#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: MIT

# Drop the SK-AM62B-P1 TDM8 device trees into a kernel source tree and register
# them in arch/arm64/boot/dts/ti/Makefile.  Idempotent: safe to re-run after
# `build-tdm8-uac2-sk.sh fetch` re-clones ./linux, and after a kernel bump.
#
# Unlike the MYIR board there is no vendor blob here - mainline carries
# k3-am625-sk.dts, so these are ordinary in-tree sources that #include it.
#
# Usage: apply-tdm8-sk-dts.sh [<linux-src>]   (default: ../../linux)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
KSRC=${1:-$(cd "$HERE/../.." && pwd)/linux}
TI="$KSRC/arch/arm64/boot/dts/ti"

DTS_LIST="k3-am625-sk-tdm8 k3-am625-sk-tdm8-j3"

[ -f "$KSRC/Makefile" ] || { echo "not a kernel tree: $KSRC" >&2; exit 1; }
[ -f "$TI/k3-am625-sk.dts" ] || {
	echo "$TI/k3-am625-sk.dts missing - the TDM8 device trees #include it" >&2
	exit 1
}

for d in $DTS_LIST; do
	cp "$HERE/$d.dts" "$TI/$d.dts"
done

# Register next to k3-am625-sk.dtb so `make dtbs` builds them too.  A direct
# `make ti/<name>.dtb` works without this, but only the Makefile entry makes
# them part of a full dtbs build and of `make dtbs_install`.
for d in $DTS_LIST; do
	if grep -q "^dtb-\$(CONFIG_ARCH_K3) += $d.dtb$" "$TI/Makefile"; then
		echo "Makefile: $d.dtb already registered"
		continue
	fi
	awk -v line="dtb-\$(CONFIG_ARCH_K3) += $d.dtb" '
	{ print }
	/^dtb-\$\(CONFIG_ARCH_K3\) \+= k3-am625-sk\.dtb$/ { print line }
	' "$TI/Makefile" > "$TI/Makefile.tdm8" && mv "$TI/Makefile.tdm8" "$TI/Makefile"
	echo "Makefile: added $d.dtb"
done

echo "TDM8 SK device trees applied to $KSRC"
