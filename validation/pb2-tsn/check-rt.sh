#!/bin/sh

# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic

# Scheduling latency of the PB2 under load (check F1.2).
#
# Runs cyclictest with one measuring thread per CPU, the isolated ones included
# (the media plane's CPU 3 is the one that matters most), optionally alongside
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

# judge a cyclictest -q -h output: per-CPU min/avg/max, histogram overflows.
# The sample count is the "# Total:" line, or, from versions that print none
# (2.80), the sum of the histogram rows.
judge() { # <file> <max_us>
	awk -v lim="$2" '
		/^[0-9]+[ \t]/ {
			for (i = 2; i <= NF; i++) {
				sum[i - 2] += $i
			}
		}
		/^# Min Latencies:/ { for (i = 4; i <= NF; i++) mn[i - 4] = $i + 0; n = NF - 3 }
		/^# Avg Latencies:/ { for (i = 4; i <= NF; i++) av[i - 4] = $i + 0 }
		/^# Max Latencies:/ { for (i = 4; i <= NF; i++) mx[i - 4] = $i + 0; have = 1 }
		/^# Histogram Overflows:/ { for (i = 4; i <= NF; i++) ov[i - 4] = $i + 0 }
		/^# Total:/ { for (i = 3; i <= NF; i++) tot[i - 3] = $i + 0 }
		END {
			if (!have) { print "no \"# Max Latencies\" line: cyclictest did not finish"; print "RESULT: FAIL"; exit 1 }
			bad = 0; worst = 0
			for (c = 0; c < n; c++) {
				if (!(c in tot)) {
					tot[c] = sum[c]
				}

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

	# cyclictest 2.80 on the PB2: no "# Total:" line, so the histogram gives
	# the count (10 min at 200 us, 3 CPUs: about 3 million samples each)
	out=$(judge "$HERE/samples/cyclictest-nototal.txt" 100)
	r=$?
	if [ "$r" -eq 1 ] && ! echo "$out" | grep -q 'samples 0 '; then
		echo "check-rt self-test: sample without a total -> FAIL, counted ok"
	else
		echo "check-rt self-test: sample without a total -> exit $r WRONG"
		rc=1
	fi
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
# mainline PREEMPT_RT names itself in the version string; /sys/kernel/realtime
# came with the out-of-tree patch set only
case "$(uname -v)" in
*PREEMPT_RT*) RT=yes ;;
*) [ "$(cat /sys/kernel/realtime 2>/dev/null)" = 1 ] && RT=yes || RT=no ;;
esac
echo "realtime:  $RT"
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
# -S puts one thread on each CPU of the process's affinity, which isolcpus
# leaves out of every task's default: widen it to all of them first
NCPU=$(grep -c '^processor' /proc/cpuinfo)
ALL="0-$((NCPU - 1))"

WIDEN=
if command -v taskset >/dev/null 2>&1; then
	WIDEN="taskset -c $ALL"
	echo "cpus:      $ALL (isolated: $(cat /sys/devices/system/cpu/isolated 2>/dev/null))"
else
	echo "cpus:      taskset missing, the isolated CPUs are not measured"
fi

echo "run:       $WIDEN cyclictest -m -S -p $PRIO -i $INTERVAL -h $HIST_MAX -q -D $DURATION > $OUT"
$WIDEN cyclictest \
	-m \
	-S \
	-p "$PRIO" \
	-i "$INTERVAL" \
	-h "$HIST_MAX" \
	-q \
	-D "$DURATION" \
	> "$OUT" 2>&1
stop_load
judge "$OUT" "$MAX_US"
