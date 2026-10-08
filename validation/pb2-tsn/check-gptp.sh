#!/bin/sh

# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic

# gPTP state and offset statistics of ptp4l on the PB2 (checks F2.2, F2.3, F2.5).
#
# State, through pmc on the ptp4l socket: port state, asCapable, GM present and
# its identity. Offsets, either sampled from TIME_STATUS_NP for --window seconds
# or parsed from a ptp4l log (-m output, one "master offset" line per Sync or
# "rms ... max ..." summaries). Only samples after the first lock are judged.
#
# usage: check-gptp.sh [--uds /var/run/ptp4lro] [--role slave|master] [--expect-gm ID]
#                      [--window SECONDS] [--interval SECONDS] [--log FILE]
#                      [--offset-ns 100] [--fraction 99.9] [--excursion-ns 1000]
#                      [--delay-spread-ns 50] [--pmc-file FILE]
#        check-gptp.sh --self-test
#
#   --window 0 checks the state only; --log judges a ptp4l log instead of sampling;
#   --pmc-file judges a saved pmc output instead of asking ptp4l.
#
# Exit 0 = pass, 1 = fail, 2 = usage error.

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SELF=$HERE/$(basename "$0")
UDS=/var/run/ptp4lro
ROLE=slave
EXPECT_GM=
WINDOW=0
INTERVAL=1
LOG=
OFFSET_NS=100
FRACTION=99.9
EXCURSION_NS=1000
DELAY_SPREAD_NS=50
PMC_FILE=

usage() { sed -n '/^# usage:/,/^# Exit/p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

pmc_get() {
	pmc -u -b 0 -t 1 -s "$UDS" "$@" 2>/dev/null
}

# key/value pairs of every pmc response field ("portState SLAVE", ...)
pmc_fields() {
	awk 'NF >= 2 && $1 !~ /^(sending:|[0-9a-f]+\.[0-9a-f]+\.[0-9a-f]+-[0-9]+)$/ { print $1, $2 }'
}

# judge the state fields read from pmc
judge_state() { # <file with pmc output>
	awk -v role="$ROLE" -v gm="$EXPECT_GM" '
		NF >= 2 { v[$1] = $2 }
		END {
			bad = 0
			want = (role == "master") ? "MASTER" : "SLAVE"
			ps = ("portState" in v) ? v["portState"] : "-"
			printf "portState %s (want %s)\n", ps, want
			if (ps != want) bad++
			ac = ("asCapable" in v) ? v["asCapable"] : "-"
			printf "asCapable %s (want 1)\n", ac
			if (ac != "1") bad++
			gp = ("gmPresent" in v) ? v["gmPresent"] : "-"
			gi = ("gmIdentity" in v) ? v["gmIdentity"] : (("grandmasterIdentity" in v) ? v["grandmasterIdentity"] : "-")
			printf "gmPresent %s gmIdentity %s", gp, gi
			if (gm != "") printf " (want %s)", gm
			printf "\n"
			if (role == "slave" && gp != "true") bad++
			if (gm != "" && tolower(gi) != tolower(gm)) bad++
			if ("master_offset" in v) printf "master_offset %s ns\n", v["master_offset"]
			if ("peerMeanPathDelay" in v) printf "peerMeanPathDelay %s ns\n", v["peerMeanPathDelay"]
			print (bad ? "STATE: FAIL" : "STATE: PASS")
			exit (bad ? 1 : 0)
		}' "$1"
}

# judge offset/delay/state samples. Input lines: "o <offset_ns>", "d <delay_ns>",
# "s <port state>", "r <rms> <max>" (a summary), "l" (first lock seen)
judge_offsets() {
	awk -v lim="$OFFSET_NS" -v frac="$FRACTION" -v exc="$EXCURSION_NS" -v spread="$DELAY_SPREAD_NS" '
		function abs(x) { return x < 0 ? -x : x }
		$1 == "o" { n++; a = abs($2 + 0); if (a <= lim) ok++; if (a > worst) worst = a; if (a > exc) over++ }
		$1 == "r" { sn++; m = $3 + 0; if (m > worst) worst = m; if (m > exc) over++; if (m <= lim) sok++ }
		$1 == "d" { dn++; x = $2 + 0; if (dn == 1 || x < dmin) dmin = x; if (dn == 1 || x > dmax) dmax = x }
		$1 == "s" { if (last != "" && $2 != last) changes++; last = $2 }
		$1 == "c" { changes++ }
		END {
			bad = 0
			if (n + sn == 0) { print "no offset sample after lock"; print "OFFSETS: FAIL"; exit 1 }
			if (n) {
				pct = 100.0 * ok / n
				printf "offset samples %d: |offset| <= %d ns for %.3f%% (want >= %s%%), worst %d ns, over %d ns: %d\n", \
					n, lim, pct, frac, worst, exc, over
				if (pct < frac) bad++
			} else {
				printf "summaries %d (no per-Sync lines): worst max %d ns, summaries with max <= %d ns: %d, over %d ns: %d\n", \
					sn, worst, lim, sok, exc, over
				if (sok < sn) bad++
			}
			if (over) bad++
			if (dn) {
				printf "path delay %d samples: min %d max %d ns, half spread %.1f ns (want <= %d)\n", \
					dn, dmin, dmax, (dmax - dmin) / 2.0, spread
				if ((dmax - dmin) / 2.0 > spread) bad++
			}
			printf "port state changes after lock: %d (want 0)\n", changes + 0
			if (changes) bad++
			print (bad ? "OFFSETS: FAIL" : "OFFSETS: PASS")
			exit (bad ? 1 : 0)
		}'
}

# turn a ptp4l -m log into judge_offsets input, starting at the first lock
log_samples() { # <log>
	awk '
		/ to SLAVE on / || / to MASTER on / || / to GRAND_MASTER on / { if (!locked) { locked = 1; print "l"; next } }
		locked && / port [0-9]+.*: [A-Z_]+ to [A-Z_]+ on / { print "c"; next }
		locked && /master offset/ {
			for (i = 1; i <= NF; i++) {
				if ($i == "offset") off = $(i + 1)
				if ($i == "delay") dl = $(i + 1)
			}
			if ($0 ~ / s2 /) { print "o", off + 0; if (dl != "") print "d", dl + 0 }
			next
		}
		locked && / rms / && / max / {
			for (i = 1; i <= NF; i++) {
				if ($i == "rms") r = $(i + 1)
				if ($i == "max") m = $(i + 1)
				if ($i == "delay") dl = $(i + 1)
			}
			print "r", r + 0, m + 0
			if (dl != "") print "d", dl + 0
		}' "$1"
}

sample_window() {
	end=$(( $(date +%s) + WINDOW ))
	while [ "$(date +%s)" -lt "$end" ]; do
		pmc_get 'GET TIME_STATUS_NP' 'GET PORT_DATA_SET' | awk '
			$1 == "master_offset" { print "o", $2 }
			$1 == "peerMeanPathDelay" { print "d", $2 }
			$1 == "portState" { print "s", $2 }'
		sleep "$INTERVAL"
	done
}

self_test() {
	rc=0
	S=$HERE/samples
	expect() { # <want rc> <label> <cmd...>
		w=$1; l=$2; shift 2
		"$@" >/dev/null 2>&1; r=$?
		if [ "$r" -eq "$w" ]; then echo "check-gptp self-test: $l -> exit $r ok"; else echo "check-gptp self-test: $l -> exit $r, want $w WRONG"; rc=1; fi
	}
	expect 0 "pmc slave sample" sh "$SELF" --pmc-file "$S/pmc-slave.txt" --expect-gm 3cc0c6.fffe.fe0210
	expect 1 "pmc wrong GM" sh "$SELF" --pmc-file "$S/pmc-slave.txt" --expect-gm 001122.fffe.334455
	expect 1 "pmc listening sample" sh "$SELF" --pmc-file "$S/pmc-listening.txt"
	expect 0 "ptp4l log pass sample" sh "$SELF" --pmc-file "$S/pmc-slave.txt" --log "$S/ptp4l-pass.log"
	expect 1 "ptp4l log fail sample" sh "$SELF" --pmc-file "$S/pmc-slave.txt" --log "$S/ptp4l-fail.log"
	expect 0 "ptp4l summary-only sample" sh "$SELF" --pmc-file "$S/pmc-slave.txt" --log "$S/ptp4l-summary.log"
	exit "$rc"
}

while [ $# -gt 0 ]; do
	case "$1" in
	--uds)             UDS=${2:?}; shift ;;
	--role)            ROLE=${2:?}; shift ;;
	--expect-gm)       EXPECT_GM=${2:?}; shift ;;
	--window)          WINDOW=${2:?}; shift ;;
	--interval)        INTERVAL=${2:?}; shift ;;
	--log)             LOG=${2:?}; shift ;;
	--offset-ns)       OFFSET_NS=${2:?}; shift ;;
	--fraction)        FRACTION=${2:?}; shift ;;
	--excursion-ns)    EXCURSION_NS=${2:?}; shift ;;
	--delay-spread-ns) DELAY_SPREAD_NS=${2:?}; shift ;;
	--pmc-file)        PMC_FILE=${2:?}; shift ;;
	--self-test)       self_test ;;
	-h|--help)         usage ;;
	*) echo "check-gptp: unknown argument $1" >&2; usage ;;
	esac
	shift
done
case "$ROLE" in slave|master) ;; *) echo "check-gptp: --role slave|master" >&2; exit 2 ;; esac

TMP=${TMPDIR:-/tmp}/check-gptp.$$
trap 'rm -f "$TMP".*' EXIT INT TERM

if [ -n "$PMC_FILE" ]; then
	[ -r "$PMC_FILE" ] || { echo "check-gptp: cannot read $PMC_FILE" >&2; exit 2; }
	pmc_fields < "$PMC_FILE" > "$TMP.state"
else
	command -v pmc >/dev/null 2>&1 || { echo "check-gptp: pmc missing (linuxptp)" >&2; exit 2; }
	pmc_get 'GET PORT_DATA_SET' 'GET PORT_DATA_SET_NP' 'GET TIME_STATUS_NP' 'GET PARENT_DATA_SET' \
		| pmc_fields > "$TMP.state"
	[ -s "$TMP.state" ] || { echo "check-gptp: no answer from ptp4l on $UDS" >&2; exit 1; }
fi
judge_state "$TMP.state"; rs=$?

ro=0
if [ -n "$LOG" ]; then
	[ -r "$LOG" ] || { echo "check-gptp: cannot read $LOG" >&2; exit 2; }
	log_samples "$LOG" | judge_offsets; ro=$?
elif [ "$WINDOW" -gt 0 ]; then
	echo "sampling TIME_STATUS_NP every ${INTERVAL}s for ${WINDOW}s"
	sample_window | judge_offsets; ro=$?
fi

if [ "$rs" -eq 0 ] && [ "$ro" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
