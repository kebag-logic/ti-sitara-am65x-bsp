#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0

# Run tests/rxfail.py: milan-ctrld on veth0, milan-mediad started again for
# each receive setup failure rxfail.so injects, the controller on veth1, in a
# private user and network namespace (unshare -rn), so it needs no root and
# touches no real interface.
#
# usage: run-netns-rxfail.sh <build dir> [entity.conf]

set -e
HERE=$(cd "$(dirname "$0")" && pwd)

if [ "$1" != --inside ]; then
	B=$(realpath "${1:?usage: run-netns-rxfail.sh <build dir> [entity.conf]}")
	ENTITY=$(realpath "${2:-$HERE/../config/entity.conf}")
	exec unshare -rn sh "$0" --inside "$B" "$ENTITY"
fi
B=$2; ENTITY=$3
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

ip link set lo up
ip link add veth0 address 02:1a:2b:3c:4d:5e type veth peer name veth1 address 02:00:00:00:00:02
ip link set veth0 up
ip link set veth1 up
sleep 0.2
python3 -I "$HERE/rxfail.py" --dut veth0 --peer veth1 --build "$B" --entity "$ENTITY" --tmp "$TMP"
