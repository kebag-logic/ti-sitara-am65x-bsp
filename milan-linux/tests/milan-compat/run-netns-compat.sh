#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0

# The board's entity as a la_avdecc controller sees it: milan-ctrld and
# milan-mediad (talker, AAF listener and CRF input, on a simulated USB host)
# on veth0, flexptpd's status block played by gptp.py, and milan-compat
# (milan_compat.cpp, on la_avdecc 4.3.x over pcap) on veth1, in a private user
# and network namespace (unshare -rn). milan-compat enumerates the entity with
# the library's compatibility checks, prints every compatibility event and
# every warning or error it logs, and exits 0 only for a Milan-compatible
# entity with none.
#
# With "streams", veth0, veth1 and a third port, veth2, hang off a bridge, and
# talker.py on veth2 binds STREAM_INPUT 0 to an AAF stream and STREAM_INPUT 1
# to a CRF stream it sends, so the controller enumerates bound inputs whose
# counters move.
#
# MEDIAD_ROLES sets milan-mediad's -r (talker,listener,crf).
#
# usage: run-netns-compat.sh <build dir> <milan-compat> [entity.conf] [settle seconds] [streams]

set -e
HERE=$(cd "$(dirname "$0")" && pwd)

if [ "$1" != --inside ]; then
	B=$(realpath "${1:?usage: run-netns-compat.sh <build dir> <milan-compat> [entity.conf] [settle]}")
	COMPAT=$(realpath "${2:?}")
	ENTITY=$(realpath "${3:-$HERE/../../config/entity.conf}")
	exec unshare -rn sh "$0" --inside "$B" "$COMPAT" "$ENTITY" "${4:-15}" "${5:-}"
fi
B=$2; COMPAT=$3; ENTITY=$4; SETTLE=$5; MODE=$6
TMP=$(mktemp -d)
DP=/milan-dp-compat-$$; MD=/milan-media-compat-$$; G=flexptpd-compat-$$
cleanup() {
	kill $PIDS 2>/dev/null || true
	wait 2>/dev/null || true
	rm -f "/dev/shm$DP" "/dev/shm$MD" "/dev/shm/$G"
	rm -rf "$TMP"
}
trap cleanup EXIT

ip link set lo up
if [ "$MODE" = streams ]; then
	ip link add br0 type bridge mcast_snooping 0
	ip link set br0 up
	for p in 0 1 2; do
		case $p in 0) MAC=02:1a:2b:3c:4d:5e ;; 1) MAC=02:00:00:00:00:02 ;; 2) MAC=02:00:00:00:00:03 ;; esac
		ip link add veth$p address $MAC type veth peer name port$p
		ip link set port$p master br0
		ip link set port$p up
		ip link set veth$p up
	done
else
	ip link add veth0 address 02:1a:2b:3c:4d:5e type veth peer name veth1 address 02:00:00:00:00:02
	ip link set veth0 up
	ip link set veth1 up
fi
sleep 0.2

python3 -I "$HERE/gptp.py" "$G" 021a2bfffe3c4d5e &
PIDS="$!"
sleep 0.3
"$B/milan-ctrld" -i veth0 -e "$ENTITY" -g "$G" -n -d "$DP" -m "$MD" -N "$TMP/journal.bin" > "$TMP/ctrld.log" 2>&1 &
PIDS="$PIDS $!"
# with streams, the PTO and interruption threshold talker.py's streams are sent for (tests/counters.py)
MEDIAD_OPT=
[ "$MODE" = streams ] && MEDIAD_OPT="-o 50000000 -T 200000000"
# shellcheck disable=SC2086
"$B/milan-mediad" -i veth0 -r "${MEDIAD_ROLES:-talker,listener,crf}" -S 0 -P 0 $MEDIAD_OPT -d "$DP" -m "$MD" > "$TMP/mediad.log" 2>&1 &
PIDS="$PIDS $!"
sleep 2
if [ "$MODE" = streams ]; then
	python3 -I "$HERE/talker.py" veth2 021a2b3c4d5e > "$TMP/talker.log" 2>&1 &
	TALKER=$!
	PIDS="$PIDS $TALKER"
	sleep 3
fi
echo "--- milan-compat on veth1, for entity 021a2bfffe3c4d5e ---"
rc=0
"$COMPAT" veth1 --entity 021a2bfffe3c4d5e --wait 20 --settle "$SETTLE" || rc=$?
echo "--- milan-dp ---"
"$B/milan-dp" "$DP" "$MD" | grep -E '^(sink|source|milan_stream|link|gm|maap)' || true
kill $PIDS 2>/dev/null || true
wait 2>/dev/null || true
PIDS=
for f in ctrld mediad talker; do [ -f "$TMP/$f.log" ] && { echo "--- $f.log"; cat "$TMP/$f.log"; }; done
echo "compat: $([ "$rc" -eq 0 ] && echo PASS || echo FAIL)"
exit "$rc"
