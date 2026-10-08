#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0

# Run tests/peer.py against milan-ctrld across a veth pair, in a private user
# and network namespace (unshare -rn), so it needs no root and touches no real
# interface. veth0 is the DUT (02:00:00:00:00:01), veth1 the peer.
#
# usage: run-netns.sh <milan-ctrld> [milan-dp] [entity.conf]

set -e
HERE=$(cd "$(dirname "$0")" && pwd)

if [ "$1" != --inside ]; then
	CTRLD=$(realpath "${1:?usage: run-netns.sh <milan-ctrld> [milan-dp] [entity.conf]}")
	DP=$(realpath "${2:-$(dirname "$CTRLD")/milan-dp}")
	ENTITY=$(realpath "${3:-$HERE/../config/entity.conf}")
	exec unshare -rn sh "$0" --inside "$CTRLD" "$DP" "$ENTITY"
fi
CTRLD=$2; DP=$3; ENTITY=$4
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

ip link set lo up
ip link add veth0 address 02:00:00:00:00:01 type veth peer name veth1 address 02:00:00:00:00:02
ip link set veth0 up
ip link set veth1 up
# veth reports carrier once both ends are up
sleep 0.2
python3 -I "$HERE/peer.py" --dut veth0 --peer veth1 --ctrld "$CTRLD" --dp "$DP" --entity "$ENTITY" --tmp "$TMP"
