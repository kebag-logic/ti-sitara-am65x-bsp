#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0

# The saved state on the host (issue #13): milan-fpga's KLJ2 store on a journal
# file. Bridge A talks on veth0; bridge B's milan-ctrld keeps its journal.
#   1. a controller binds B to A; B's milan-ctrld is killed with SIGKILL (a power
#      cut, nothing flushed) once the store has written; restarted, B settles
#      on A's stream again with no controller: a fast connect (A6.1)
#   2. unbound, killed, restarted: B stays unbound (A6.2)
#   3. bind and unbind in a loop, SIGKILL at a random point, restart, every
#      round: the store always boots on an accepted slot, never a torn one (A6.3)
#
# usage: run-netns-nvm.sh <build dir> [rounds]

set -e
HERE=$(cd "$(dirname "$0")" && pwd)

if [ "$1" != --inside ]; then
	B=$(realpath "${1:?usage: run-netns-nvm.sh <build dir> [rounds]}")
	exec unshare -rn sh "$0" --inside "$B" "${2:-20}"
fi
B=$2; ROUNDS=$3
TMP=$(mktemp -d)
DPA=/milan-dp-na-$$; DPB=/milan-dp-nb-$$
E="$HERE/../config/entity.conf"
cleanup() {
	kill $A $CB 2>/dev/null || true
	wait 2>/dev/null || true
	rm -f "/dev/shm$DPA" "/dev/shm$DPB"
	rm -rf "$TMP"
}
trap cleanup EXIT

ip link set lo up
ip link add veth0 address 02:00:00:00:00:01 type veth peer name veth1 address 02:00:00:00:00:02
ip link set veth0 up
ip link set veth1 up
sleep 0.2

"$B/milan-ctrld" -i veth0 -e "$E" -p none -n -l "$TMP/a.ptp" -d "$DPA" -N "$TMP/a.bin" > "$TMP/a.log" 2>&1 &
A=$!
# start B, and wait for its own datapath block: until then the block is the
# killed process's, and must not be read for the new one's state
start_b() {
	"$B/milan-ctrld" -i veth1 -e "$E" -p none -n -l "$TMP/b.ptp" -d "$DPB" -N "$TMP/b.bin" >> "$TMP/b.log" 2>&1 &
	CB=$!
	i=0
	while [ "$i" -lt 50 ] && [ "$("$B/milan-dp" "$DPB" 2>/dev/null | sed -n 's/^writer_pid=//p')" != "$CB" ]; do
		sleep 0.05; i=$((i + 1))
	done
}
fail=0
check() { if [ "$2" -eq 0 ]; then echo "[PASS] $1: $3"; else echo "[FAIL] $1: $3"; fail=1; fi; }
sink() { "$B/milan-dp" "$DPB" 2>/dev/null | sed -n 's/^sink0=//p'; }
listening() { sink | grep -q listening:1; }
wait_listening() { # <seconds>
	i=0; while [ "$i" -lt $(($1 * 10)) ] && ! listening; do sleep 0.1; i=$((i + 1)); done; listening
}
boots() { grep -c '^saved state at boot' "$TMP/b.log"; }
last_boot() { grep '^saved state at boot' "$TMP/b.log" | tail -1; }

start_b
sleep 3        # MAAP on both, before the talker can answer a probe

# 1. bind, let the store write (1 s debounce), power cut, fast connect
python3 -I "$HERE/bind.py" veth0 020000fffe000002 020000fffe000001 0 0 >/dev/null
check "bound" "$(wait_listening 3; echo $?)" "$(sink)"
sleep 3
kill -9 "$CB"; wait "$CB" 2>/dev/null || true
t0=$(date +%s%N)
start_b
ok=$(wait_listening 5; echo $?)
t1=$(date +%s%N)
check "A6.1 binding survives a power cut; fast connect without a controller" \
	"$([ "$ok" = 0 ] && last_boot | grep -q 'bindings restored'; echo $?)" \
	"settled $(( (t1 - t0) / 1000000 )) ms after the restart; $(last_boot)"

# 2. unbind, power cut: stays unbound
python3 -I - <<'PY'
import socket, struct
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3)); s.bind(("veth0", 3))
pdu = struct.pack(">BBHQQQQHH", 0xFC, 8, 44, 0, 0x0200C0FFEE000001, 0x020000FFFE000001, 0x020000FFFE000002, 0, 0) \
    + b"\0" * 6 + struct.pack(">HHHHH", 0, 0x7171, 0, 0, 0)
s.send(bytes.fromhex("91e0f0010000") + s.getsockname()[4] + struct.pack(">H", 0x22F0) + pdu)
PY
sleep 3
kill -9 "$CB"; wait "$CB" 2>/dev/null || true
start_b
sleep 5
check "A6.2 an unbind survives a power cut" "$(listening && echo 1 || echo 0)" "$(sink)"

# 3. power cuts at random points of bind/unbind, every round restarted
bad=0
r=0
while [ "$r" -lt "$ROUNDS" ]; do
	if [ $((r % 2)) -eq 0 ]; then
		python3 -I "$HERE/bind.py" veth0 020000fffe000002 020000fffe000001 0 0 >/dev/null 2>&1 || true
	else
		python3 -I - <<'PY'
import socket, struct
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3)); s.bind(("veth0", 3))
pdu = struct.pack(">BBHQQQQHH", 0xFC, 8, 44, 0, 0x0200C0FFEE000001, 0x020000FFFE000001, 0x020000FFFE000002, 0, 0) \
    + b"\0" * 6 + struct.pack(">HHHHH", 0, 0x7272, 0, 0, 0)
s.send(bytes.fromhex("91e0f0010000") + s.getsockname()[4] + struct.pack(">H", 0x22F0) + pdu)
PY
	fi
	# 0.8 to 1.6 s: before, during and after the store's write
	sleep "1.$(( (r * 37) % 7 ))"
	kill -9 "$CB"; wait "$CB" 2>/dev/null || true
	start_b
	sleep 0.5
	if ! kill -0 "$CB" 2>/dev/null || ! last_boot | grep -qE 'slot (A|B) ' || last_boot | grep -q 'read faults [1-9]' ||
	   ! last_boot | grep -q 'bindings restored'; then
		bad=$((bad + 1))
		echo "  round $r: $(last_boot)"
	fi
	r=$((r + 1))
done
check "A6.3 $ROUNDS power cuts: every boot judged an accepted slot, no read fault" "$bad" \
	"$(boots) boots, last: $(last_boot)"
# the journal still works after them: a bind is written and survives
python3 -I "$HERE/bind.py" veth0 020000fffe000002 020000fffe000001 0 0 >/dev/null 2>&1 || true
sleep 3
kill -9 "$CB"; wait "$CB" 2>/dev/null || true
start_b
check "after them, a bind still survives a power cut" "$(wait_listening 5; echo $?)" "$(sink)"

if [ "$fail" -ne 0 ]; then
	echo "--- B ---"; tail -40 "$TMP/b.log"
	echo "nvm: FAIL"
	exit 1
fi
echo "nvm: PASS"
