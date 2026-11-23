#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0

# Generate every file milan-ctrld reads about its entity from the board's
# end-station config, through milan-fpga's own generators, so none of them is
# a restated copy:
#
#   config/entity.conf        the ADP fields (tools/gen-entity-conf.sh, through
#                             sw/firmware/ctrl/adp/adp_entity.py)
#   config/aecp_entity_gen.h  the AEM image AECP serves, its CRC and storage
#                             sizes (sw/firmware/ctrl/aecp/aecp_entity.py)
#   config/srp_entity_gen.h   the shape header every firmware unit is compiled
#                             with (sw/firmware/ctrl/srp/srp_entity.py)
#   config/nvm_shape_gen.h    the saved-state store's shape
#                             (tools/gen-nvm-shape.py, through nvm_bench.py)
#
# The milan-fpga tree needs its protocol-processor and gptp-processor trees
# (tools/fetch-milan-fpga.sh --with-submodules), and PyYAML.
# With --check, the files are generated into a temporary directory and compared
# with config/: exit 1 naming each one that differs.
#
# usage: gen-config.sh [--check] [<milan-fpga tree> [<end-station yaml>]]

set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
CHECK=no
if [ "$1" = --check ]; then
	CHECK=yes
	shift
fi
TREE=$(cd "${1:-$HERE/.milan-fpga}" && pwd)
YAML=$(realpath "${2:-$HERE/config/endstation_pb2_bridge.yaml}")
OUT=$HERE/config
if [ "$CHECK" = yes ]; then
	OUT=$(mktemp -d)
	trap 'rm -rf "$OUT"' EXIT
fi
CTRL=$TREE/sw/firmware/ctrl

"$HERE/tools/gen-entity-conf.sh" "$TREE" "$YAML" > "$OUT/entity.conf"
python3 -B "$CTRL/aecp/aecp_entity.py" "$YAML" -o "$OUT/aecp_entity_gen.h"
python3 -B "$CTRL/srp/srp_entity.py" "$YAML" -o "$OUT/srp_entity_gen.h"
python3 -B "$HERE/tools/gen-nvm-shape.py" "$TREE" "$YAML" > "$OUT/nvm_shape_gen.h"

if [ "$CHECK" = yes ]; then
	stale=0
	for f in entity.conf aecp_entity_gen.h srp_entity_gen.h nvm_shape_gen.h; do
		if ! cmp -s "$OUT/$f" "$HERE/config/$f"; then
			echo "config/$f is not what $(basename "$YAML") generates at milan-fpga $(cat "$TREE/.milan-fpga-rev" 2>/dev/null || echo unknown): run tools/gen-config.sh" >&2
			stale=1
		fi
	done
	[ "$stale" = 0 ] && echo "config/: every generated file is current for $(basename "$YAML")"
	exit "$stale"
fi
echo "config/: entity.conf, aecp_entity_gen.h, srp_entity_gen.h, nvm_shape_gen.h from $(basename "$YAML")"
