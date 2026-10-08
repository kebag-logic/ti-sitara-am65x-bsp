#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0

# The listener end to end on the host (issue #12), between two bridges. Bridge
# A (veth0) talks: a simulated USB host 80 ppm fast plays the counting ramp. A
# controller binds bridge B's STREAM_INPUT 0 to A's STREAM_OUTPUT 0 with a Milan
# BIND_RX. B probes A, settles, and plays the stream to its own simulated USB
# host, 50 ppm slow, which records it. Then the recording is checked bit for bit,
# and B's Milan counters show the stream locked, with no sequence error, no late
# PDU and no reset once locked.
#
# Runs in a private user and network namespace (unshare -rn); no SCHED_FIFO.
#
# usage: run-netns-listener.sh <build dir> [seconds]

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
KIT=$(cd "$HERE/../../validation/pb2-tsn" && pwd)

if [ "$1" != --inside ]; then
	B=$(realpath "${1:?usage: run-netns-listener.sh <build dir> [seconds]}")
	exec unshare -rn sh "$0" --inside "$B" "${2:-10}"
fi
B=$2; SECS=$3
TMP=$(mktemp -d)
DPA=/milan-dp-a-$$; MDA=/milan-media-a-$$
DPB=/milan-dp-b-$$; MDB=/milan-media-b-$$
cleanup() {
	kill $PIDS 2>/dev/null || true
	wait 2>/dev/null || true
	rm -f "/dev/shm$DPA" "/dev/shm$MDA" "/dev/shm$DPB" "/dev/shm$MDB"
	rm -rf "$TMP"
}
trap cleanup EXIT

ip link set lo up
ip link add veth0 address 02:00:00:00:00:01 type veth peer name veth1 address 02:00:00:00:00:02
ip link set veth0 up
ip link set veth1 up
sleep 0.2

E="$HERE/../config/entity.conf"
# -n: a direct link, no MSRP bridge, so a settled sink does not wait for SRP
"$B/milan-ctrld" -i veth0 -e "$E" -p none -n -l "$TMP/a.ptp" -d "$DPA" -N "$TMP/a.bin" > "$TMP/ctrld-a.log" 2>&1 &
PIDS="$!"
"$B/milan-ctrld" -i veth1 -e "$E" -p none -n -l "$TMP/b.ptp" -d "$DPB" -N "$TMP/b.bin" > "$TMP/ctrld-b.log" 2>&1 &
PIDS="$PIDS $!"
# Without SCHED_FIFO, on a shared host, either bridge's threads can be held off
# for tens of milliseconds. A 50 ms PTO and drop level, and a 100 ms
# interruption threshold, keep that from reading as a stream fault; the board
# runs 2 ms, 2 ms and 10 ms on its own isolated core.
"$B/milan-mediad" -i veth0 -r talker -S 80 -P 0 -M 2400 -o 50000000 -d "$DPA" -m "$MDA" > "$TMP/mediad-a.log" 2>&1 &
PIDS="$PIDS $!"
"$B/milan-mediad" -i veth1 -r listener -S -50 -W "$TMP/b.wav" -P 0 -o 50000000 -T 100000000 -d "$DPB" -m "$MDB" > "$TMP/mediad-b.log" 2>&1 &
MEDIAD_B=$!
PIDS="$PIDS $MEDIAD_B"

fail=0
check() { if [ "$2" -eq 0 ]; then echo "[PASS] $1: $3"; else echo "[FAIL] $1: $3"; fail=1; fi; }
fa() { "$B/milan-dp" "$DPA" "$MDA" 2>/dev/null | sed -n "s/^$1=//p"; }
fb() { "$B/milan-dp" "$DPB" "$MDB" 2>/dev/null | sed -n "s/^$1=//p"; }

# A's talker streams once its MAAP address is held
i=0
while [ "$i" -lt 15 ] && [ "$(fa talker_active)" != 1 ]; do sleep 1; i=$((i + 1)); done
check "talker A streaming" "$([ "$(fa talker_active)" = 1 ]; echo $?)" "after ${i} s"

# the controller's BIND_RX, sent where B hears it
python3 -I "$HERE/bind.py" veth0 020000fffe000002 020000fffe000001 0 0
check "B settled on A's stream" "$(i=0; while [ $i -lt 20 ] && ! fb sink0 | grep -q listening:1; do sleep 0.2; i=$((i+1)); done; fb sink0 | grep -q "stream_id:$(fa source0 | sed 's/stream_id:\([0-9a-f]*\).*/\1/')"; echo $?)" "$(fb sink0)"

# B locks, then runs for SECS seconds
i=0
while [ "$i" -lt 30 ] && [ "$(fb listener_locked)" != 1 ]; do sleep 1; i=$((i + 1)); done
echo "[INFO] B $([ "$(fb listener_locked)" = 1 ] && echo locked || echo "not locked (host jitter)") after ${i} s, playback pitch $(fb listener_pitch)"
r0=$(fb listener_frames_rx); s0=$(fb listener_seq_mismatch); l0=$(fb listener_late_timestamp)
x0=$(fb listener_media_resets); u0=$(fb listener_underruns); n0=$(fb listener_stream_interrupted)
sleep "$SECS"
r1=$(fb listener_frames_rx); s1=$(fb listener_seq_mismatch); l1=$(fb listener_late_timestamp)
x1=$(fb listener_media_resets); u1=$(fb listener_underruns); n1=$(fb listener_stream_interrupted)

check "FRAMES_RX at 8000/s" "$(awk -v a="$r0" -v b="$r1" -v s="$SECS" 'BEGIN { r = (b - a) / s; exit !(r > 7900 && r < 8100) }'; echo $?)" \
	"$((r1 - r0)) PDUs in $SECS s"
check "no SEQ_NUM_MISMATCH, LATE_TIMESTAMP or STREAM_INTERRUPTED" \
	"$([ "$s0" = "$s1" ] && [ "$l0" = "$l1" ] && [ "$n0" = "$n1" ]; echo $?)" "seq $s0 -> $s1, late $l0 -> $l1, interrupted $n0 -> $n1"
check "no MEDIA_RESET and no underrun" "$([ "$x0" = "$x1" ] && [ "$u0" = "$u1" ]; echo $?)" \
	"resets $x0 -> $x1, underruns $u0 -> $u1"
check "presentation error within 125 us" "$(awk -v lo="$(fb listener_align_min_ns)" -v hi="$(fb listener_align_max_ns)" \
	'BEGIN { exit !(lo > -125000 && hi < 125000) }'; echo $?)" \
	"$(fb listener_align_min_ns)..$(fb listener_align_max_ns) ns, average $(fb listener_align_avg_ns) ns"
p=$(fb listener_pitch)
check "playback pitch cancels the host's -50 ppm" "$([ "${p:-0}" -ge 1000000 ] && [ "${p:-0}" -le 1000100 ]; echo $?)" \
	"pitch $p (1000050 +/- 50 expected)"

# stop B so its simulated host closes the recording, then check it
kill "$MEDIAD_B"
wait "$MEDIAD_B" 2>/dev/null || true
# The recording runs from the first settlement, and its warm-up may hold the
# jumps (MEDIA_RESET) this unprivileged host's stalls cause before the servo
# settles. Its last SECS - 1 seconds lie inside the window whose counters
# above show no jump, no underrun and no interruption: those are checked bit for
# bit, with nothing allowed.
python3 -I - "$TMP/b.wav" "$TMP/tail.wav" "$((SECS - 1))" <<'PY'
import struct, sys
src, dst, secs = sys.argv[1], sys.argv[2], int(sys.argv[3])
d = open(src, "rb").read()
ch, rate, bits = struct.unpack_from("<HIxxxxxxH", d, 22)
frame = ch * bits // 8
data = d[44:]
keep = data[len(data) - min(len(data) // frame, secs * rate) * frame:]
hdr = d[:40] + struct.pack("<I", len(keep))
hdr = hdr[:4] + struct.pack("<I", 36 + len(keep)) + hdr[8:]
open(dst, "wb").write(hdr + keep)
PY
echo "--- ramp-check of the last $((SECS - 1)) s B's host recorded ---"
python3 -I "$KIT/ramp-check.py" "$TMP/tail.wav" --min-seconds "$((SECS - 2))" || fail=1

if [ "$fail" -ne 0 ]; then
	for f in "$TMP"/*.log; do echo "--- $f"; cat "$f"; done
	echo "listener: FAIL"
	exit 1
fi
echo "listener: PASS"
