#!/bin/sh

# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic

# Self-test of the validation kit itself (checks V1.1, V1.2, V1.3), on a host
# with python3. No board, no network.
#
#   V1.1  every script gives the expected verdict on the committed samples,
#         including one failing sample per script
#   V1.2  latency-peer.py reports known injected latencies within 1 us
#         (nanosecond and microsecond captures, across a 32-bit AVTP wrap)
#   V1.3  ramp-check.py finds one dropped frame, one repeated frame and one
#         flipped bit, each at its position, in a 10-minute 8-channel ramp
#
# usage: selftest.sh [--skip-v13]
#   BUSYBOX=/path/to/busybox selftest.sh   runs the board scripts under BusyBox ash/awk
#
# Exit 0 = every check passed, 1 = a check failed.

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
S=$HERE/samples
PY="python3 -I"
SKIP_V13=no
[ "${1:-}" = --skip-v13 ] && SKIP_V13=yes
FAILS=0
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT INT TERM

ok()   { echo "  ok    $*"; }
bad()  { echo "  FAIL  $*"; FAILS=$((FAILS + 1)); }

# run a command, compare its exit code
expect() { # <want rc> <label> <cmd...>
	w=$1; l=$2; shift 2
	"$@" > "$T/out" 2>&1; r=$?
	if [ "$r" -eq "$w" ]; then ok "$l (exit $r)"; else bad "$l: exit $r, want $w"; sed 's/^/        /' "$T/out" | tail -5; fi
}

# the board scripts: plain sh, or BusyBox applets first in PATH
SH=sh
if [ -n "${BUSYBOX:-}" ]; then
	mkdir -p "$T/bb"
	for a in sh awk sed cat dirname basename rm mkdir sort tail head grep tr; do ln -s "$BUSYBOX" "$T/bb/$a"; done
	SH="env PATH=$T/bb:$PATH $T/bb/sh"
	echo "board scripts run under $BUSYBOX"
fi

echo "V1.1 every script on the committed samples"
for s in check-rt.sh check-gptp.sh check-shaper.sh; do
	expect 0 "$s --self-test" $SH "$HERE/$s" --self-test
done
expect 0 "aaf-analyze pass sample (A3.1/A3.2 criteria)" $PY "$HERE/aaf-analyze.py" "$S/aaf-pass.pcap" \
	--stream 020000fffe000012 --expect-rate 8000 --rate-tol-ppm 2000 --expect-step-ns 125000 \
	--expect-format INT_32BIT --expect-channels 8 --expect-nsr 48000 --expect-spf 6 --expect-bit-depth 32 \
	--expect-vid 2 --expect-pcp 3 --max-arrival-dev-us 5 --wav "$T/pass.wav"
expect 1 "aaf-analyze fail sample (gap, tv=0, step)" $PY "$HERE/aaf-analyze.py" "$S/aaf-fail.pcap" \
	--expect-step-ns 125000 --wav "$T/fail.wav" --stream 020000fffe000013
expect 1 "aaf-analyze pass sample against the wrong format" $PY "$HERE/aaf-analyze.py" "$S/aaf-pass.pcap" \
	--expect-channels 2
expect 2 "aaf-analyze usage error" $PY "$HERE/aaf-analyze.py" "$T/missing.pcap"
expect 0 "ramp-check on the payload of the pass capture" $PY "$HERE/ramp-check.py" "$T/pass.wav"
expect 1 "ramp-check on the payload of the fail capture" $PY "$HERE/ramp-check.py" "$T/fail.wav"
expect 0 "ramp-check pass sample" $PY "$HERE/ramp-check.py" "$S/ramp-pass.wav" --min-seconds 0.09
expect 1 "ramp-check fail sample" $PY "$HERE/ramp-check.py" "$S/ramp-fail.wav"
expect 2 "ramp-check usage error" $PY "$HERE/ramp-check.py" "$S/aaf-pass.pcap"
$PY "$HERE/ramp-check.py" "$S/ramp-fail.wav" --json > "$T/rf.json" 2>&1
if $PY -c 'import json,sys; r=json.load(open(sys.argv[1])); e=r["events"][0]
sys.exit(0 if (r["dropped_frames"], e["kind"], e["frame"], e["first_missing_counter"]) == (1, "drop", 1064, 1000) else 1)' "$T/rf.json"
then ok "ramp-check names the planted drop: file frame 1064, counter 1000"; else bad "ramp-check drop position"; cat "$T/rf.json"; fi
$PY "$HERE/ramp-gen.py" "$T/regen.wav" --channels 2 --frames 4800 --lead-silence 64 > /dev/null
if cmp -s "$T/regen.wav" "$S/ramp-pass.wav"; then ok "ramp-gen reproduces ramp-pass.wav byte for byte"; else bad "ramp-gen output differs from ramp-pass.wav"; fi
expect 2 "ramp-gen usage error" $PY "$HERE/ramp-gen.py" "$T/x.wav" --channels 0
expect 0 "latency-peer pass sample" $PY "$HERE/latency-peer.py" "$S/latency-pass.pcap"
expect 1 "latency-peer fail sample (one 2.5 ms frame)" $PY "$HERE/latency-peer.py" "$S/latency-fail.pcap"
expect 2 "latency-peer usage error" $PY "$HERE/latency-peer.py" "$T/missing.pcap"

echo "V1.2 latency-peer.py against known injected latencies"
$PY "$S/make-samples.py" --v12 "$T/v12"
for res in ns us; do
	$PY "$HERE/latency-peer.py" "$T/v12/known-$res.pcap" --allow-negative --max-us 1e9 --csv "$T/v12/got-$res.csv" > /dev/null
	if $PY -c 'import sys
k = [l.split(",") for l in open(sys.argv[1]).read().split()[1:]]
g = [l.split(",") for l in open(sys.argv[2]).read().split()[1:]]
if len(k) != len(g): print(f"{len(g)} frames reported, {len(k)} injected"); sys.exit(1)
err = [abs(int(b[4]) - int(a[2])) for a, b in zip(k, g)]
seq = all(int(a[1]) == int(b[1]) for a, b in zip(k, g))
print(f"{len(err)} frames, max |error| {max(err)} ns, sequence order {seq}")
sys.exit(0 if max(err) <= 1000 and seq else 1)' "$T/v12/known.csv" "$T/v12/got-$res.csv" > "$T/cmp" 2>&1
	then ok "$res capture: $(cat "$T/cmp")"; else bad "$res capture: $(cat "$T/cmp")"; fi
done
expect 1 "latency-peer fails on the negative latency without --allow-negative" \
	$PY "$HERE/latency-peer.py" "$T/v12/known-ns.pcap" --max-us 1e9

if [ "$SKIP_V13" = yes ]; then
	echo "V1.3 skipped (--skip-v13)"
else
	echo "V1.3 ramp-check.py on a 10-minute 8-channel ramp with three planted defects"
	$PY "$HERE/ramp-gen.py" "$T/r10.wav" --channels 8 --rate 48000 --seconds 600 \
		--drop-frame 5000000 --repeat-frame 12000000 --flip 20000000:5:17 > /dev/null
	$PY "$HERE/ramp-check.py" "$T/r10.wav" --json > "$T/r10.json"
	if $PY -c 'import json, sys
r = json.load(open(sys.argv[1]))
ev = {e["kind"]: e for e in r["events"]}
want = [
  ("dropped frames 1", r["dropped_frames"] == 1),
  ("repeated frames 1", r["repeated_frames"] == 1),
  ("bit errors only ch5, 1 bit", r["bit_errors_per_channel"] == [0, 0, 0, 0, 0, 1, 0, 0]),
  ("drop at file frame 5000000, counter 5000000", ev.get("drop", {}).get("frame") == 5000000 and ev["drop"]["first_missing_counter"] == 5000000),
  ("repeat at file frame 12000000, counter 12000000", ev.get("repeat", {}).get("frame") == 12000000 and ev["repeat"]["counter"] == 12000000),
  ("flip at file frame 20000000, counter 20000000, ch 5 bit 17", ev.get("bit-error", {}).get("frame") == 20000000 and ev["bit-error"]["counter"] == 20000000 and ev["bit-error"]["bits"] == [[5, 17]]),
  ("3 events, 600 s checked", r["events_total"] == 3 and r["ramp_seconds_checked"] >= 599.99),
]
for name, good in want: print(("ok " if good else "BAD ") + name)
sys.exit(0 if all(g for _, g in want) else 1)' "$T/r10.json" > "$T/cmp" 2>&1
	then sed 's/^ok /  ok    /' "$T/cmp"; else sed 's/^/        /' "$T/cmp"; bad "V1.3 positions"; fi
	rm -f "$T/r10.wav"
fi

if [ "$FAILS" -eq 0 ]; then echo "SELFTEST: PASS"; exit 0; fi
echo "SELFTEST: FAIL ($FAILS)"
exit 1
