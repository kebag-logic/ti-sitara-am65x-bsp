#!/bin/sh

# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic

# gPTP state and offsets of flexptpd on the PB2 (checks F2.2, F2.3, F2.5, #4).
#
# State, from flexptpd's status block (milan-dp -g): the port state, asCapable,
# a grandmaster present and its identity.
#
# Offsets, from the wire, over --window seconds: milan-gptp-watch takes the
# board's hardware receive time of each Sync and the time its Follow_Up
# carries, whatever the daemon claims. Over the same window the status block
# is sampled every --interval seconds: the port state must never change,
# asCapable must stay 1, and the link delay must stay within
# +/- --delay-spread-ns of its centre.
#
# usage: check-gptp.sh [--interface eth0] [--name flexptpd.IF] [--role slave|master]
#                      [--expect-gm ID] [--window SECONDS] [--interval SECONDS]
#                      [--link-delay-ns 203] [--offset-ns 100] [--delay-spread-ns 50]
#                      [--status-file FILE] [--watch-file FILE]
#        check-gptp.sh --self-test
#
#   ID as 3cc0c6.fffe.fe0210 or 3cc0c6fffefe0210. --window 0 checks the state
#   only. --status-file judges a saved `milan-dp -g` output instead of the
#   block, --watch-file a saved milan-gptp-watch output instead of the wire.
#
# Exit 0 = pass, 1 = fail, 2 = usage error.

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SELF=$HERE/$(basename "$0")

IF=eth0
NAME=
ROLE=slave
EXPECT_GM=
WINDOW=0
INTERVAL=1
LINK_DELAY_NS=203
OFFSET_NS=100
DELAY_SPREAD_NS=50
STATUS_FILE=
WATCH_FILE=

usage() {
	sed -n '/^# usage:/,/^# Exit/p' "$0" | sed 's/^# \{0,1\}//' >&2
	exit 2
}

# the status, as key=value lines
status_now() {
	if [ -n "$STATUS_FILE" ]; then
		cat "$STATUS_FILE"
	else
		milan-dp -g "$NAME" 2>/dev/null
	fi
}

field() { # <key> < status
	sed -n "s/^$1=//p"
}

# 3cc0c6.fffe.fe0210 or 3cc0c6fffefe0210 -> 3cc0c6fffefe0210
plain_id() {
	echo "$1" | tr -d '.:' | tr 'A-F' 'a-f'
}

# F2.2: the port's state now
check_state() {
	st=$(status_now)

	if [ -z "$st" ]; then
		echo "no gPTP status block $NAME (is flexptpd running?)"
		echo "STATE: FAIL"
		return 1
	fi

	want=SLAVE
	if [ "$ROLE" = master ]; then
		want=MASTER
	fi

	ps=$(echo "$st" | field port_state)
	ac=$(echo "$st" | field as_capable)
	gp=$(echo "$st" | field gm_present)
	gm=$(echo "$st" | field gm_identity)
	own=$(echo "$st" | field own_identity)
	delay=$(echo "$st" | field mean_link_delay_ns)

	echo "portState $ps (want $want)"
	echo "asCapable $ac (want 1)"
	echo "gmPresent $gp gmIdentity $gm"
	echo "meanLinkDelay $delay ns"

	ok=1
	[ "$ps" = "$want" ] || ok=0
	[ "$ac" = 1 ] || ok=0
	[ "$gp" = 1 ] || ok=0

	if [ "$ROLE" = master ]; then
		# the grandmaster is us
		[ "$gm" = "$own" ] || ok=0
	elif [ -n "$EXPECT_GM" ]; then
		echo "expected grandmaster $(plain_id "$EXPECT_GM")"
		[ "$gm" = "$(plain_id "$EXPECT_GM")" ] || ok=0
	fi

	if [ "$ok" = 1 ]; then
		echo "STATE: PASS"
		return 0
	fi

	echo "STATE: FAIL"
	return 1
}

# the status samples of the window: state steady, asCapable kept, link delay in its band
judge_samples() { # <file of "state ascapable delay" lines>
	awk -v spread="$DELAY_SPREAD_NS" '
		{
			n++
			if (n == 1) { first = $1 }
			if ($1 != first) { changes++ }
			if ($2 != 1) { notcap++ }
			if (n == 1 || $3 < lo) { lo = $3 }
			if (n == 1 || $3 > hi) { hi = $3 }
		}
		END {
			if (n == 0) { print "no status samples"; print "SAMPLES: FAIL"; exit 1 }
			half = (hi - lo) / 2
			printf "status samples %d: port state changes %d (want 0), asCapable lost in %d (want 0)\n", n, changes + 0, notcap + 0
			printf "link delay %d..%d ns, half spread %.1f ns (want <= %d)\n", lo, hi, half, spread
			ok = (changes + 0 == 0) && (notcap + 0 == 0) && (half <= spread)
			print (ok ? "SAMPLES: PASS" : "SAMPLES: FAIL")
			exit (ok ? 0 : 1)
		}' "$1"
}

# F2.3: the offsets of the window, from the wire
check_window() {
	tmp=$(mktemp -d)

	if [ -n "$WATCH_FILE" ]; then
		cp "$WATCH_FILE" "$tmp/watch.txt"
		status_now | awk -F= '
			$1 == "port_state" { s = $2 }
			$1 == "as_capable" { a = $2 }
			$1 == "mean_link_delay_ns" { d = $2 }
			END { print s, a, d }' > "$tmp/samples.txt"
	else
		gm_opt=
		if [ -n "$EXPECT_GM" ]; then
			gm_opt="-g $(echo "$EXPECT_GM" | tr 'A-F' 'a-f')"
		fi

		milan-gptp-watch -i "$IF" -d "$WINDOW" -l "$LINK_DELAY_NS" -t "$OFFSET_NS" -q $gm_opt > "$tmp/watch.txt" 2>&1 &
		wpid=$!

		# the status every interval while the watch runs
		while kill -0 "$wpid" 2>/dev/null; do
			status_now | awk -F= '
				$1 == "port_state" { s = $2 }
				$1 == "as_capable" { a = $2 }
				$1 == "mean_link_delay_ns" { d = $2 }
				END { if (s != "") print s, a, d }' >> "$tmp/samples.txt"
			sleep "$INTERVAL"
		done

		wait "$wpid"
	fi

	sed -n '/^grandmaster\|^samples\|^offset\|^|offset|/p' "$tmp/watch.txt"

	wrc=1
	if grep -q '^RESULT: PASS' "$tmp/watch.txt"; then
		wrc=0
		echo "OFFSETS: PASS"
	else
		echo "OFFSETS: FAIL"
	fi

	judge_samples "$tmp/samples.txt"
	src=$?

	rm -rf "$tmp"

	[ "$wrc" -eq 0 ] && [ "$src" -eq 0 ]
}

self_test() {
	S=$HERE/samples
	rc=0

	run() { # <want> <label> <args...>
		want=$1
		label=$2
		shift 2

		"$SELF" "$@" >/dev/null 2>&1
		r=$?

		if [ "$r" -eq "$want" ]; then
			echo "check-gptp self-test: $label -> exit $r ok"
		else
			echo "check-gptp self-test: $label -> exit $r, want $want WRONG"
			rc=1
		fi
	}

	run 0 "slave of the expected grandmaster" --status-file "$S/gptp-status-slave.txt" --expect-gm 3cc0c6.fffe.fe0210
	run 1 "slave of another grandmaster" --status-file "$S/gptp-status-slave.txt" --expect-gm 3cc0c6.fffe.fe0211
	run 1 "listening, not asCapable" --status-file "$S/gptp-status-listening.txt"
	run 0 "the PB2 as grandmaster" --status-file "$S/gptp-status-master.txt" --role master
	run 0 "a passing 10 min watch" --status-file "$S/gptp-status-slave.txt" --window 600 --watch-file "$S/gptp-watch-pass.txt"
	run 1 "a watch with offsets beyond 100 ns" --status-file "$S/gptp-status-slave.txt" --window 600 --watch-file "$S/gptp-watch-fail.txt"

	exit "$rc"
}

while [ $# -gt 0 ]; do
	case "$1" in
	--interface)       IF=${2:?}; shift ;;
	--name)            NAME=${2:?}; shift ;;
	--role)            ROLE=${2:?}; shift ;;
	--expect-gm)       EXPECT_GM=${2:?}; shift ;;
	--window)          WINDOW=${2:?}; shift ;;
	--interval)        INTERVAL=${2:?}; shift ;;
	--link-delay-ns)   LINK_DELAY_NS=${2:?}; shift ;;
	--offset-ns)       OFFSET_NS=${2:?}; shift ;;
	--delay-spread-ns) DELAY_SPREAD_NS=${2:?}; shift ;;
	--status-file)     STATUS_FILE=${2:?}; shift ;;
	--watch-file)      WATCH_FILE=${2:?}; shift ;;
	--self-test)       self_test ;;
	-h|--help)         usage ;;
	*) echo "check-gptp: unknown argument $1" >&2; usage ;;
	esac
	shift
done

case "$ROLE" in
slave|master) ;;
*) usage ;;
esac

[ -n "$NAME" ] || NAME="flexptpd.$IF"

check_state
state_rc=$?

if [ "$WINDOW" -eq 0 ]; then
	echo "RESULT: $([ "$state_rc" -eq 0 ] && echo PASS || echo FAIL)"
	exit "$state_rc"
fi

check_window
window_rc=$?

if [ "$state_rc" -eq 0 ] && [ "$window_rc" -eq 0 ]; then
	echo "RESULT: PASS"
	exit 0
fi

echo "RESULT: FAIL"
exit 1
