#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0

# Fetch the milan-fpga tree milan-fpga.pin names into DIR (default
# milan-linux/.milan-fpga), checking the archive's sha256. With
# --with-submodules, also the protocol-processor and gptp-processor trees that
# tools/gen-entity-conf.sh needs. The Buildroot package fetches the same archive
# on its own (br2-external/package/milan-bridge); this is for host builds.
#
# usage: fetch-milan-fpga.sh [DIR] [--with-submodules]

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

if [ -f "$DIR/.milan-fpga-rev" ] && [ "$(cat "$DIR/.milan-fpga-rev")" = "$MILAN_FPGA_REV" ]; then
	echo "milan-fpga $MILAN_FPGA_REV already in $DIR"
else
	fetch "$MILAN_FPGA_REPO" "$MILAN_FPGA_REV" "$DIR" "$MILAN_FPGA_SHA256"
	echo "$MILAN_FPGA_REV" > "$DIR/.milan-fpga-rev"
	echo "milan-fpga $MILAN_FPGA_REV -> $DIR"
fi
if [ "$SUBS" = yes ]; then
	fetch "$PROTOCOL_PROCESSOR_REPO" "$PROTOCOL_PROCESSOR_REV" "$DIR/protocol-processor"
	fetch "$GPTP_PROCESSOR_REPO" "$GPTP_PROCESSOR_REV" "$DIR/gptp-processor"
	echo "submodules protocol-processor $PROTOCOL_PROCESSOR_REV, gptp-processor $GPTP_PROCESSOR_REV"
fi
