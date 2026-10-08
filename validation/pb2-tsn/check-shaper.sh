#!/bin/sh

# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic

# Readback of the CPSW egress shaper on the PB2 (checks F3.1, F3.4).
#
#   - the root qdisc is mqprio, offloaded, in channel mode with the bw_rlimit shaper
#   - p0-rx-ptype-rrobin is off (with it on, every host packet lands in port FIFO 0
#     and the per-priority shaper never sees class A or B traffic)
#   - over --interval seconds, the port's tx_priN counters move on the expected
#     priorities (mqprio maps traffic class N to port priority N), and the
#     tx_priN_drop counters are reported
#
# Start the test traffic (one generator per priority under test) first.
#
# usage: check-shaper.sh [--dev eth0] [--expect-tcs 3] [--expect-moving "0 1 2"]
#                        [--interval 10] [--no-drops] [--files DIR]
#        check-shaper.sh --self-test
#
#   --files DIR judges saved outputs instead of the live interface: DIR holds
#   qdisc.txt (tc -s qdisc show dev), privflags.txt (ethtool --show-priv-flags),
#   stats-before.txt and stats-after.txt (ethtool -S, --interval apart).
#
# Exit 0 = pass, 1 = fail, 2 = usage error.

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SELF=$HERE/$(basename "$0")
DEV=eth0
EXPECT_TCS=3
EXPECT_MOVING="0 1 2"
INTERVAL=10
NO_DROPS=no
FILES=

usage() { sed -n '/^# usage:/,/^# Exit/p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

judge() { # <qdisc> <privflags> <stats-before> <stats-after>
	bad=0
	awk -v tcs="$EXPECT_TCS" '
		/^qdisc mqprio / && / root / { root = 1; line = $0
			if ($0 ~ / offloaded/) off = 1
			for (i = 1; i <= NF; i++) if ($i == "tc") n = $(i + 1) }
		root && /mode:channel/ { mode = 1 }
		root && /shaper:bw_rlimit/ { shp = 1; s = $0; sub(/^[ \t]*/, "", s); rates = s }
		END {
			bad = 0
			printf "root mqprio %s, offloaded %s, tc %s (want %s), mode:channel %s, shaper:bw_rlimit %s\n", \
				root ? "yes" : "NO", off ? "yes" : "NO", n == "" ? "-" : n, tcs, mode ? "yes" : "NO", shp ? "yes" : "NO"
			if (rates != "") print "  " rates
			if (!root || !off || n != tcs || !mode || !shp) bad = 1
			exit bad
		}' "$1" || bad=1
	rr=$(awk -F: '$1 ~ /p0-rx-ptype-rrobin/ { gsub(/[ \t]/, "", $2); print $2 }' "$2")
	echo "p0-rx-ptype-rrobin ${rr:--} (want off)"
	[ "$rr" = off ] || bad=1
	awk -v want="$EXPECT_MOVING" -v nodrops="$NO_DROPS" '
		FNR == 1 { f++ }
		{ k = $1; sub(/:$/, "", k) }
		k ~ /^tx_pri[0-7]$/ || k ~ /^tx_pri[0-7]_drop$/ { v[f, k] = $2 + 0 }
		END {
			bad = 0
			split(want, w, " ")
			for (p = 0; p < 8; p++) {
				d = v[2, "tx_pri" p] - v[1, "tx_pri" p]
				dd = v[2, "tx_pri" p "_drop"] - v[1, "tx_pri" p "_drop"]
				ex = 0
				for (i in w) if (w[i] == p) ex = 1
				st = ""
				if (ex && d <= 0) { st = " NOT MOVING"; bad = 1 }
				if (nodrops == "yes" && dd > 0) { st = st " DROPS"; bad = 1 }
				if (d || dd || ex) printf "tx_pri%d +%d frames, +%d drops%s%s\n", p, d, dd, ex ? " (expected to move)" : "", st
			}
			exit bad
		}' "$3" "$4" || bad=1
	if [ "$bad" -eq 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
	return "$bad"
}

self_test() {
	rc=0
	for c in pass:0 fail-rrobin:1 fail-nooffload:1 fail-notmoving:1; do
		d=${c%:*}; w=${c#*:}
		sh "$SELF" --files "$HERE/samples/shaper-$d" >/dev/null 2>&1; r=$?
		if [ "$r" -eq "$w" ]; then echo "check-shaper self-test: shaper-$d -> exit $r ok"; else echo "check-shaper self-test: shaper-$d -> exit $r, want $w WRONG"; rc=1; fi
	done
	exit "$rc"
}

while [ $# -gt 0 ]; do
	case "$1" in
	--dev)           DEV=${2:?}; shift ;;
	--expect-tcs)    EXPECT_TCS=${2:?}; shift ;;
	--expect-moving) EXPECT_MOVING=${2?}; shift ;;
	--interval)      INTERVAL=${2:?}; shift ;;
	--no-drops)      NO_DROPS=yes ;;
	--files)         FILES=${2:?}; shift ;;
	--self-test)     self_test ;;
	-h|--help)       usage ;;
	*) echo "check-shaper: unknown argument $1" >&2; usage ;;
	esac
	shift
done

if [ -n "$FILES" ]; then
	for f in qdisc.txt privflags.txt stats-before.txt stats-after.txt; do
		[ -r "$FILES/$f" ] || { echo "check-shaper: missing $FILES/$f" >&2; exit 2; }
	done
	judge "$FILES/qdisc.txt" "$FILES/privflags.txt" "$FILES/stats-before.txt" "$FILES/stats-after.txt"
	exit $?
fi

for t in tc ethtool; do
	command -v "$t" >/dev/null 2>&1 || { echo "check-shaper: $t missing" >&2; exit 2; }
done
T=${TMPDIR:-/tmp}/check-shaper.$$
mkdir -p "$T"
trap 'rm -rf "$T"' EXIT INT TERM
tc -s qdisc show dev "$DEV" > "$T/qdisc.txt"
ethtool --show-priv-flags "$DEV" > "$T/privflags.txt"
ethtool -S "$DEV" > "$T/stats-before.txt"
sleep "$INTERVAL"
ethtool -S "$DEV" > "$T/stats-after.txt"
tc -s class show dev "$DEV" 2>/dev/null | awk '/^class mqprio/ { c = $3 } / Sent / && c != "" { print "class " c ": sent " $2 " bytes " $4 " pkt (since qdisc creation)"; c = "" }'
judge "$T/qdisc.txt" "$T/privflags.txt" "$T/stats-before.txt" "$T/stats-after.txt"
