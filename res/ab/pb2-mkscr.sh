#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: MIT

# Wrap a U-Boot script as the PocketBeagle 2's /boot.scr (issue #27).
#
# The board's U-Boot has FIT_SIGNATURE and no LEGACY_IMAGE_FORMAT, so
# `source`, which bootstd's script bootmeth runs it with, turns a plain
# `mkimage -T script` image away ("Wrong image format for "source" command")
# and the board falls through to "nothing to boot". It takes a FIT:
#
#   - with no image named, `source` runs the default configuration's
#     "script", once its hash checks out;
#   - the board's device tree carries no /signature key, so nothing has to
#     be signed.
#
# build-tdm8-uac2-pb2.sh abcard and res/ab/test-pb2-bootchooser.sh both make
# their boot.scr here, so the sandbox sources the same format the board does.
#
# Usage: pb2-mkscr.sh <mkimage> <boot.cmd> <boot.scr>
set -e

MKIMAGE=${1:?usage: $0 <mkimage> <boot.cmd> <boot.scr>}
CMD=${2:?usage: $0 <mkimage> <boot.cmd> <boot.scr>}
OUT=${3:?usage: $0 <mkimage> <boot.cmd> <boot.scr>}

# mkimage compiles the .its with "dtc" from PATH: the U-Boot build's own when
# the host has none
if ! command -v dtc >/dev/null; then
	PATH="$(cd "$(dirname "$MKIMAGE")/.." && pwd)/scripts/dtc:$PATH"
fi

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

cp "$CMD" "$W/boot.cmd"

cat > "$W/boot.its" <<EOF
/dts-v1/;

/ {
	description = "PB2 A/B bootchooser";
	#address-cells = <1>;

	images {
		bootchooser {
			description = "PB2 A/B bootchooser";
			data = /incbin/("$W/boot.cmd");
			type = "script";
			compression = "none";

			hash-1 {
				algo = "sha256";
			};
		};
	};

	configurations {
		default = "conf-1";

		conf-1 {
			description = "PB2 A/B bootchooser";
			script = "bootchooser";
		};
	};
};
EOF

"$MKIMAGE" -f "$W/boot.its" "$W/boot.scr" >/dev/null

cp "$W/boot.scr" "$OUT"
