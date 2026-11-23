#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0

# Run tests/aecp.py, a controller enumerating milan-ctrld's entity over AECP and
# binding its two STREAM_INPUTs over ACMP, across a veth pair in a private user
# and network namespace (unshare -rn), so it needs no root and touches no real
# interface. veth0 is the DUT, veth1 the controller. The DUT's MAC,
# 02:1a:2b:3c:4d:5e, is not the generated image's placeholder
# (config/endstation_pb2_bridge.yaml), so the identity checks hold only if
# milan-ctrld writes the board's own identity into its image.
#
# usage: run-netns-aecp.sh <milan-ctrld> [milan-dp] [entity.conf]

set -e
HERE=$(cd "$(dirname "$0")" && pwd)

if [ "$1" != --inside ]; then
	CTRLD=$(realpath "${1:?usage: run-netns-aecp.sh <milan-ctrld> [milan-dp] [entity.conf]}")
	DP=$(realpath "${2:-$(dirname "$CTRLD")/milan-dp}")
	ENTITY=$(realpath "${3:-$HERE/../config/entity.conf}")
	exec unshare -rn sh "$0" --inside "$CTRLD" "$DP" "$ENTITY"
fi
CTRLD=$2; DP=$3; ENTITY=$4
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

ip link set lo up
ip link add veth0 address 02:1a:2b:3c:4d:5e type veth peer name veth1 address 02:00:00:00:00:02
ip link set veth0 up
ip link set veth1 up
# veth reports carrier once both ends are up
sleep 0.2
python3 -I "$HERE/aecp.py" --dut veth0 --peer veth1 --ctrld "$CTRLD" --dp "$DP" --entity "$ENTITY" --tmp "$TMP"
