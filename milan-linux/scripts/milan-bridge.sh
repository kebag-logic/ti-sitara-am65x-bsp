#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0

# Start, stop and report the USB-to-Milan bridge daemons: what S95avb runs for
# AVB_STACK=native, after gPTP and the shaper are up.
#
#   milan-ctrld   the Milan control plane (ADP, ACMP, MAAP; the Mark II
#                 firmware of milan-fpga on the soft fabric), SCHED_FIFO
#                 AVB_CTRLD_PRIO, logging to syslog
#   milan-mediad  the media plane: the AAF talker fed by the UAC2 gadget, on the
#                 media clock, its servo steering the host; its talker thread at
#                 SCHED_FIFO AVB_MEDIAD_PRIO on AVB_RT_CPU
#
# "status" logs both daemons' state to syslog (SIGUSR1) and prints the
# datapath and media blocks (milan-dp).
#
# usage: milan-bridge.sh {start|stop|restart|status}

ENV_FILE=${AVB_ENV:-/etc/avb/avb.env}
if [ -r "$ENV_FILE" ]; then . "$ENV_FILE"; fi

IF=${AVB_INTERFACE:-eth0}
VID=${AVB_VLAN_ID:-2}
PRIO=${AVB_CTRLD_PRIO:-40}
MPRIO=${AVB_MEDIAD_PRIO:-70}
CPU=${AVB_RT_CPU:-3}
ENTITY=${AVB_ENTITY_CONF:-/etc/milan/entity.conf}
PTO=${AVB_PTO_NS:-2000000}
LEVEL=${AVB_TALKER_LEVEL:-24}
RUN=/run/avb

pidf() { echo "$RUN/$1.pid"; }
running() { [ -f "$(pidf "$1")" ] && kill -0 "$(cat "$(pidf "$1")")" 2>/dev/null; }

daemon() { # <name> <args...>
	n=$1; shift
	if running "$n"; then
		echo "milan-bridge: $n already running (pid $(cat "$(pidf "$n")"))"
		return 0
	fi
	start-stop-daemon -S -b -m -p "$(pidf "$n")" -x "/usr/sbin/$n" -- "$@"
	sleep 1
	if ! running "$n"; then
		echo "milan-bridge: $n exited at once (see /var/log/messages)" >&2
		return 1
	fi
}

halt() { # <name>
	running "$1" || return 0
	start-stop-daemon -K -s TERM -q -p "$(pidf "$1")"
	i=0
	while running "$1" && [ "$i" -lt 20 ]; do
		sleep 0.1
		i=$((i + 1))
	done
	rm -f "$(pidf "$1")"
}

start() {
	mkdir -p "$RUN"
	daemon milan-ctrld -i "$IF" -e "$ENTITY" -V "$VID" -s || return 1
	chrt -f -p "$PRIO" "$(cat "$(pidf milan-ctrld)")" >/dev/null
	echo "milan-bridge: milan-ctrld pid $(cat "$(pidf milan-ctrld)") on $IF, SCHED_FIFO $PRIO"
	# the talker thread sets its own priority and CPU; it waits for the gadget
	daemon milan-mediad -i "$IF" -o "$PTO" -L "$LEVEL" -P "$MPRIO" -a "$CPU" -s || return 1
	echo "milan-bridge: milan-mediad pid $(cat "$(pidf milan-mediad)"), talker SCHED_FIFO $MPRIO on CPU $CPU"
}

stop() {
	halt milan-mediad
	# SIGTERM: ADP sends ENTITY_DEPARTING before milan-ctrld exits
	halt milan-ctrld
}

status() {
	for n in milan-ctrld milan-mediad; do
		if running "$n"; then
			echo "$n: running (pid $(cat "$(pidf "$n")")), state logged to syslog"
			kill -USR1 "$(cat "$(pidf "$n")")"
		else
			echo "$n: not running"
		fi
	done
	milan-dp 2>/dev/null || true
}

case "${1:-status}" in
start)   start ;;
stop)    stop ;;
restart) stop; start ;;
status)  status ;;
*) echo "usage: $0 {start|stop|restart|status}" >&2; exit 1 ;;
esac
