#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0

# Start, stop and report the USB-to-Milan bridge daemons: what S95avb runs for
# AVB_STACK=native, after gPTP and the shaper are up.
#
#   milan-ctrld   the Milan control plane (ADP, ACMP, MAAP; the Mark II
#                 firmware of milan-fpga on the soft fabric), SCHED_FIFO
#                 AVB_CTRLD_PRIO, logging to syslog
#
# "status" logs milan-ctrld's state to syslog (SIGUSR1) and prints the
# datapath block it publishes (milan-dp).
#
# usage: milan-bridge.sh {start|stop|restart|status}

ENV_FILE=${AVB_ENV:-/etc/avb/avb.env}
if [ -r "$ENV_FILE" ]; then . "$ENV_FILE"; fi

IF=${AVB_INTERFACE:-eth0}
VID=${AVB_VLAN_ID:-2}
PRIO=${AVB_CTRLD_PRIO:-40}
ENTITY=${AVB_ENTITY_CONF:-/etc/milan/entity.conf}
RUN=/run/avb
PIDF=$RUN/milan-ctrld.pid

running() { [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null; }

start() {
	mkdir -p "$RUN"
	if running; then
		echo "milan-bridge: milan-ctrld already running (pid $(cat "$PIDF"))"
		return 0
	fi
	start-stop-daemon -S -b -m -p "$PIDF" -x /usr/sbin/milan-ctrld -- \
		-i "$IF" -e "$ENTITY" -V "$VID" -s
	sleep 1
	if ! running; then
		echo "milan-bridge: milan-ctrld exited at once (see /var/log/messages)" >&2
		return 1
	fi
	chrt -f -p "$PRIO" "$(cat "$PIDF")" >/dev/null
	echo "milan-bridge: milan-ctrld pid $(cat "$PIDF") on $IF, SCHED_FIFO $PRIO"
}

stop() {
	running || return 0
	# SIGTERM: ADP sends ENTITY_DEPARTING before the daemon exits
	start-stop-daemon -K -s TERM -q -p "$PIDF"
	i=0
	while running && [ "$i" -lt 20 ]; do
		sleep 0.1
		i=$((i + 1))
	done
	rm -f "$PIDF"
}

status() {
	if running; then
		echo "milan-ctrld: running (pid $(cat "$PIDF")), state logged to syslog"
		kill -USR1 "$(cat "$PIDF")"
	else
		echo "milan-ctrld: not running"
	fi
	milan-dp 2>/dev/null || true
}

case "${1:-status}" in
start)   start ;;
stop)    stop ;;
restart) stop; start ;;
status)  status ;;
*) echo "usage: $0 {start|stop|restart|status}" >&2; exit 1 ;;
esac
