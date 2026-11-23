#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0

# Fetch the milan-fpga tree milan-fpga.pin names into DIR (default
# milan-linux/.milan-fpga), checking the archive's sha256, the tsn-c-stack
# tree under it that the firmware's ADP, ACMP, MAAP and wire layer come from,
# and lwSRP, whose headers AECP's bridge includes.
# With --with-submodules, also the protocol-processor and gptp-processor trees
# that the generators in tools/gen-config.sh need. The Buildroot package
# fetches the same archive on its own (br2-external/package/milan-bridge);
# this is for host builds.
#
# A provisional pin (MILAN_FPGA_SHA256=provisional) names a revision that has
# no published archive yet. It is taken from a local git checkout instead,
# MILAN_FPGA_LOCAL=<milan-fpga checkout>, by `git archive` of the pinned
# revision and of each submodule at its gitlink there; nothing is checked
# against a sha256, and the script says so.
#
# usage: [MILAN_FPGA_LOCAL=<checkout>] fetch-milan-fpga.sh [DIR] [--with-submodules]

set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
. "$HERE/milan-fpga.pin"
DIR="$HERE/.milan-fpga"
SUBS=no
for a in "$@"; do
	case "$a" in
	--with-submodules) SUBS=yes ;;
	-*) echo "usage: $0 [DIR] [--with-submodules]" >&2; exit 2 ;;
	*) DIR=$a ;;
	esac
done

fetch() { # <repo> <rev> <dir> [sha256]
	tmp=$(mktemp)
	curl -sSfL -o "$tmp" "$1/archive/$2.tar.gz"
	if [ -n "$4" ]; then
		echo "$4  $tmp" | sha256sum -c --quiet - || { rm -f "$tmp"; echo "sha256 mismatch: $1 $2" >&2; exit 1; }
	fi
	rm -rf "$3"
	mkdir -p "$3"
	tar -xzf "$tmp" --strip-components=1 -C "$3"
	rm -f "$tmp"
}

local_tree() { # <git checkout> <rev> <dir>
	git -C "$1" rev-parse -q --verify "$2^{commit}" >/dev/null ||
		{ echo "$1 has no commit $2" >&2; exit 1; }
	rm -rf "$3"
	mkdir -p "$3"
	git -C "$1" archive "$2" | tar -x -C "$3"
}

local_sub() { # <superproject checkout> <path>: the submodule at the pinned gitlink
	rev=$(git -C "$MILAN_FPGA_LOCAL" ls-tree "$MILAN_FPGA_REV" "$1" | awk '{print $3}')
	[ -n "$rev" ] || { echo "milan-fpga $MILAN_FPGA_REV has no gitlink $1" >&2; exit 1; }
	local_tree "$MILAN_FPGA_LOCAL/$1" "$rev" "$DIR/$1"
	echo "  $1 $rev (from the local checkout)"
}

provisional() { [ "$MILAN_FPGA_SHA256" = provisional ]; }

if [ -f "$DIR/.milan-fpga-rev" ] && [ "$(cat "$DIR/.milan-fpga-rev")" = "$MILAN_FPGA_REV" ]; then
	echo "milan-fpga $MILAN_FPGA_REV already in $DIR"
elif provisional; then
	[ -n "$MILAN_FPGA_LOCAL" ] ||
		{ echo "the pin is provisional (no published archive): set MILAN_FPGA_LOCAL=<milan-fpga checkout>" >&2; exit 1; }
	local_tree "$MILAN_FPGA_LOCAL" "$MILAN_FPGA_REV" "$DIR"
	local_sub third_party/tsn-c-stack
	local_sub third_party/lwSRP
	echo "$MILAN_FPGA_REV" > "$DIR/.milan-fpga-rev"
	echo "milan-fpga $MILAN_FPGA_REV -> $DIR, PROVISIONAL: from $MILAN_FPGA_LOCAL, no sha256 checked"
else
	fetch "$MILAN_FPGA_REPO" "$MILAN_FPGA_REV" "$DIR" "$MILAN_FPGA_SHA256"
	fetch "$TSN_C_STACK_REPO" "$TSN_C_STACK_REV" "$DIR/third_party/tsn-c-stack" "$TSN_C_STACK_SHA256"
	fetch "$LWSRP_REPO" "$LWSRP_REV" "$DIR/third_party/lwSRP" "$LWSRP_SHA256"
	echo "$MILAN_FPGA_REV" > "$DIR/.milan-fpga-rev"
	echo "milan-fpga $MILAN_FPGA_REV -> $DIR, tsn-c-stack $TSN_C_STACK_REV, lwSRP $LWSRP_REV"
fi
if [ "$SUBS" = yes ] && [ ! -f "$DIR/protocol-processor/.fetched" ]; then
	if provisional; then
		local_sub protocol-processor
		local_sub gptp-processor
	else
		fetch "$PROTOCOL_PROCESSOR_REPO" "$PROTOCOL_PROCESSOR_REV" "$DIR/protocol-processor"
		fetch "$GPTP_PROCESSOR_REPO" "$GPTP_PROCESSOR_REV" "$DIR/gptp-processor"
	fi
	touch "$DIR/protocol-processor/.fetched"
	echo "submodules protocol-processor $PROTOCOL_PROCESSOR_REV, gptp-processor $GPTP_PROCESSOR_REV"
fi
