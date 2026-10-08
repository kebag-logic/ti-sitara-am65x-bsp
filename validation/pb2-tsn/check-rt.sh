#!/bin/sh

# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic

# Scheduling latency of the PB2 under load (check F1.2).
#
# Runs cyclictest with one measuring thread per CPU, optionally alongside
# stress-ng and an iperf3 client, and judges the per-CPU maximum against a
# threshold. The 8 ch x 48 kHz USB audio load F1.2 also asks for is started
# from the host (aplay into the gadget) before this script.
#
# usage: check-rt.sh [--duration 1h] [--max-us 100] [--interval 200] [--prio 95]
#                    [--hist-max 400] [--load] [--iperf-peer ADDR] [--out FILE]
#        check-rt.sh --parse FILE [--max-us N]     judge a saved cyclictest output
#        check-rt.sh --self-test                   parse the committed samples
#
# Exit 0 = every CPU's max <= --max-us, 1 = fail, 2 = usage error.

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
DURATION=1h
MAX_US=100
INTERVAL=200
PRIO=95
HIST_MAX=400
LOAD=no
IPERF_PEER=
OUT=
PARSE=
LOAD_PIDS=

usage() { sed -n '/^# usage:/,/^# Exit/p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

# seconds in a cyclictest/stress-ng style duration (90, 90s, 30m, 1h, 1d)
to_seconds() {
	case "$1" in
	*d) echo $(( ${1%d} * 86400 )) ;;
	*h) echo $(( ${1%h} * 3600 )) ;;
	*m) echo $(( ${1%m} * 60 )) ;;
	*s) echo "${1%s}" ;;
	*)  echo "$1" ;;
	esac
}

# judge a cyclictest -q -h output: per-CPU min/avg/max, histogram overflows
judge() { # <file> <max_us>
	awk -v lim="$2" '
		/^# Min Latencies:/ { for (i = 4; i <= NF; i++) mn[i - 4] = $i + 0; n = NF - 3 }
		/^# Avg Latencies:/ { for (i = 4; i <= NF; i++) av[i - 4] = $i + 0 }
		/^# Max Latencies:/ { for (i = 4; i <= NF; i++) mx[i - 4] = $i + 0; have = 1 }
		/^# Histogram Overflows:/ { for (i = 4; i <= NF; i++) ov[i - 4] = $i + 0 }
		/^# Total:/ { for (i = 3; i <= NF; i++) tot[i - 3] = $i + 0 }
		END {
			if (!have) { print "no \"# Max Latencies\" line: cyclictest did not finish"; print "RESULT: FAIL"; exit 1 }
			bad = 0; worst = 0
			for (c = 0; c < n; c++) {
				v = (mx[c] > lim) ? "FAIL" : "ok"
				if (mx[c] > lim) bad++
				if (mx[c] > worst) worst = mx[c]
				printf "cpu%d samples %d min %d avg %d max %d us (limit %d) overflows %d %s\n", \
					c, tot[c], mn[c], av[c], mx[c], lim, ov[c], v
			}
			printf "worst max %d us over %d CPUs\n", worst, n
			if (n == 0) { print "RESULT: FAIL"; exit 1 }
			print (bad ? "RESULT: FAIL" : "RESULT: PASS")
			exit (bad ? 1 : 0)
		}' "$1"
}

self_test() {
	rc=0
	judge "$HERE/samples/cyclictest-pass.txt" 100 >/dev/null; r=$?
	if [ "$r" -eq 0 ]; then echo "check-rt self-test: pass sample -> PASS ok"; else echo "check-rt self-test: pass sample -> exit $r WRONG"; rc=1; fi
	judge "$HERE/samples/cyclictest-fail.txt" 100 >/dev/null; r=$?
	if [ "$r" -eq 1 ]; then echo "check-rt self-test: fail sample -> FAIL ok"; else echo "check-rt self-test: fail sample -> exit $r WRONG"; rc=1; fi
	judge "$HERE/samples/cyclictest-truncated.txt" 100 >/dev/null; r=$?
	if [ "$r" -eq 1 ]; then echo "check-rt self-test: truncated sample -> FAIL ok"; else echo "check-rt self-test: truncated sample -> exit $r WRONG"; rc=1; fi
	exit "$rc"
}

stop_load() {
	for p in $LOAD_PIDS; do kill "$p" 2>/dev/null; done
	LOAD_PIDS=
}

while [ $# -gt 0 ]; do
	case "$1" in
	--duration)   DURATION=${2:?}; shift ;;
	--max-us)     MAX_US=${2:?}; shift ;;
	--interval)   INTERVAL=${2:?}; shift ;;
	--prio)       PRIO=${2:?}; shift ;;
	--hist-max)   HIST_MAX=${2:?}; shift ;;
	--load)       LOAD=yes ;;
	--iperf-peer) IPERF_PEER=${2:?}; shift ;;
	--out)        OUT=${2:?}; shift ;;
	--parse)      PARSE=${2:?}; shift ;;
	--self-test)  self_test ;;
	-h|--help)    usage ;;
	*) echo "check-rt: unknown argument $1" >&2; usage ;;
	esac
	shift
done

if [ -n "$PARSE" ]; then
	[ -r "$PARSE" ] || { echo "check-rt: cannot read $PARSE" >&2; exit 2; }
	judge "$PARSE" "$MAX_US"
	exit $?
fi

command -v cyclictest >/dev/null 2>&1 || { echo "check-rt: cyclictest missing (BR2_PACKAGE_RT_TESTS)" >&2; exit 2; }
SECS=$(to_seconds "$DURATION")
[ -n "$OUT" ] || OUT=/tmp/check-rt-$(date +%Y%m%d-%H%M%S).txt
trap stop_load EXIT INT TERM

echo "kernel:    $(uname -r) $(uname -v)"
echo "realtime:  $(cat /sys/kernel/realtime 2>/dev/null || echo 0)"
echo "cmdline:   $(cat /proc/cmdline)"
if [ "$LOAD" = yes ]; then
	if command -v stress-ng >/dev/null 2>&1; then
		stress-ng --cpu 3 --io 1 --vm 1 --vm-bytes 64M --timeout "${SECS}s" >/dev/null 2>&1 &
		LOAD_PIDS="$LOAD_PIDS $!"
		echo "load:      stress-ng --cpu 3 --io 1 --vm 1 --vm-bytes 64M"
	else
		echo "load:      stress-ng not installed, CPU/IO/VM load skipped"
	fi
fi
if [ -n "$IPERF_PEER" ]; then
	if command -v iperf3 >/dev/null 2>&1; then
		iperf3 -c "$IPERF_PEER" -t "$SECS" --bidir >/dev/null 2>&1 &
		LOAD_PIDS="$LOAD_PIDS $!"
		echo "load:      iperf3 --bidir to the peer for ${SECS}s"
	else
		echo "load:      iperf3 not installed, network load skipped"
	fi
fi
echo "run:       cyclictest -m -S -p $PRIO -i $INTERVAL -h $HIST_MAX -q -D $DURATION > $OUT"
cyclictest -m -S -p "$PRIO" -i "$INTERVAL" -h "$HIST_MAX" -q -D "$DURATION" > "$OUT" 2>&1
stop_load
judge "$OUT" "$MAX_US"
