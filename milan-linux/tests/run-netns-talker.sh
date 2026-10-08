#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0

# The talker end to end on the host (issues #10, #11): milan-ctrld acquires a
# MAAP address, milan-mediad streams a simulated USB host that runs 80 ppm fast,
# and the far end of a veth pair records the stream. Then the validation kit
# grades the capture: rate, sequence, the 125 000 ns timestamp step, the AAF
# fields, VLAN 2 PCP 3, and the counting ramp bit for bit; and the media block
# shows the servo locked with the host's offset cancelled.
#
# Runs in a private user and network namespace (unshare -rn): no root, no real
# interface, no real-time priority (the host's scheduler, not the board's).
#
# usage: run-netns-talker.sh <build dir> [seconds]

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
KIT=$(cd "$HERE/../../validation/pb2-tsn" && pwd)

if [ "$1" != --inside ]; then
	B=$(realpath "${1:?usage: run-netns-talker.sh <build dir> [seconds]}")
	exec unshare -rn sh "$0" --inside "$B" "${2:-10}"
fi
B=$2; SECS=$3
TMP=$(mktemp -d)
DP=/milan-dp-talker-$$
MED=/milan-media-talker-$$
cleanup() {
	kill "$CTRLD" "$MEDIAD" 2>/dev/null || true
	wait 2>/dev/null || true
	rm -f "/dev/shm$DP" "/dev/shm$MED"
	rm -rf "$TMP"
}
trap cleanup EXIT

ip link set lo up
ip link add veth0 address 02:00:00:00:00:01 type veth peer name veth1 address 02:00:00:00:00:02
ip link set veth0 up
ip link set veth1 up
sleep 0.2

"$B/milan-ctrld" -i veth0 -e "$HERE/../config/entity.conf" -p none -d "$DP" > "$TMP/ctrld.log" 2>&1 &
CTRLD=$!
# no SCHED_FIFO in a user namespace, so the host's stalls are absorbed by a
# 10 ms drop level; on the board the talker runs SCHED_FIFO on its own core
"$B/milan-mediad" -i veth0 -S 80 -P 0 -M 480 -d "$DP" -m "$MED" > "$TMP/mediad.log" 2>&1 &
MEDIAD=$!

fail=0
check() { # <name> <condition result 0/1> <detail>
	if [ "$2" -eq 0 ]; then echo "[PASS] $1: $3"; else echo "[FAIL] $1: $3"; fail=1; fi
}
field() { "$B/milan-dp" "$DP" "$MED" 2>/dev/null | sed -n "s/^$1=//p"; }

# Let the servo settle. Its lock (2 frames for 2 s) is a board check (A4.1):
# here the host's millisecond stalls, without SCHED_FIFO, shake the level, so
# the test asks only that the pitch cancel the host's offset.
i=0
while [ "$i" -lt 20 ] && [ "$(field talker_locked)" != 1 ]; do sleep 1; i=$((i + 1)); done
echo "[INFO] servo $([ "$(field talker_locked)" = 1 ] && echo locked || echo "not locked (host jitter)") after ${i} s, pitch $(field talker_pitch)"
u0=$(field talker_underruns); o0=$(field talker_overruns)

python3 -I "$HERE/capture.py" veth1 "$TMP/talker.pcap" "$SECS"

u1=$(field talker_underruns); o1=$(field talker_overruns)
check "every PDU sent" "$([ "$(field talker_send_errors)" = 0 ]; echo $?)" "send errors $(field talker_send_errors)"
pitch=$(field talker_pitch)
check "no underrun or overrun while recording" "$([ "$u0" = "$u1" ] && [ "$o0" = "$o1" ]; echo $?)" \
	"underruns $u0 -> $u1, overruns $o0 -> $o1"
check "pitch cancels the host's +80 ppm" "$([ "${pitch:-0}" -ge 999870 ] && [ "${pitch:-0}" -le 999970 ]; echo $?)" \
	"pitch $pitch (999920 +/- 50 expected)"
check "level held at its target" "$(awk -v a="$(field talker_level_avg)" -v t="$(field talker_level_target)" \
	'BEGIN { exit !(a - t < 3 && t - a < 3) }'; echo $?)" \
	"average $(field talker_level_avg), target $(field talker_level_target): $(awk -v a="$(field talker_level_avg)" \
	'BEGIN { printf "%.0f", a / 48 * 1000 }') us from USB packet to PDU"

echo "--- aaf-analyze ---"
python3 -I "$KIT/aaf-analyze.py" "$TMP/talker.pcap" --stream 0200000000010000 --expect-rate 8000 --rate-tol-ppm 500 \
	--expect-step-ns 125000 --step-tol-ns 1 --expect-format INT_32BIT --expect-channels 8 \
	--expect-nsr 48000 --expect-spf 6 --expect-bit-depth 32 --wav "$TMP/talker.wav" || fail=1
echo "--- ramp-check ---"
python3 -I "$KIT/ramp-check.py" "$TMP/talker.wav" || fail=1
echo "--- slot to wire (latency-peer; informational: no SCHED_FIFO on the host) ---"
python3 -I "$KIT/latency-peer.py" "$TMP/talker.pcap" --pto-ns 2000000 | grep -E 'latency us' || true

# VLAN 2, PCP 3 on every frame
python3 -I - "$TMP/talker.pcap" <<'EOF' || fail=1
import struct, sys
d = open(sys.argv[1], "rb").read()
off, n, bad = 24, 0, 0
while off + 16 <= len(d):
    _, _, cap, _ = struct.unpack_from("<IIII", d, off)
    fr = d[off + 16: off + 16 + cap]
    off += 16 + cap
    tagged = fr[12:14] == b"\x81\x00"
    sub = fr[18] if tagged else fr[14]
    if sub != 0x02:
        continue                        # ADP and MAAP go out untagged; only the AAF stream counts
    n += 1
    if not tagged or struct.unpack(">H", fr[14:16])[0] != (3 << 13) | 2:
        bad += 1
print(f"[{'PASS' if n and not bad else 'FAIL'}] VLAN 2 PCP 3: {n - bad} of {n} frames")
sys.exit(0 if n and not bad else 1)
EOF

if [ "$fail" -ne 0 ]; then
	echo "--- milan-mediad ---"; cat "$TMP/mediad.log"
	echo "--- milan-ctrld ---"; cat "$TMP/ctrld.log"
	echo "talker: FAIL"
	exit 1
fi
echo "talker: PASS"
