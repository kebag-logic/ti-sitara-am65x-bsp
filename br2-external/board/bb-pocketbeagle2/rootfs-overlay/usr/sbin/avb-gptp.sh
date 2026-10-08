#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0

# gPTP (802.1AS) on the PocketBeagle 2 Ethernet Cap (issues #4, #19):
# flexptpd, our IEEE 802.1AS-2020 end station (kebag-logic/flexPTP), on the
# CPTS hardware clock of eth0.
#
# It steers the PHC onto the grandmaster and publishes the port's state in
# /dev/shm/flexptpd.<interface>, which milan-ctrld reads for ADP and
# `milan-dp -g` prints. Nothing steers the system clocks: the media plane
# models the PHC itself. The daemon's event log goes to AVB_GPTP_LOG.
#
# usage: avb-gptp.sh {start|stop|restart|status}

set -e

ENV_FILE=${AVB_ENV:-/etc/avb/avb.env}
if [ -r "$ENV_FILE" ]; then . "$ENV_FILE"; fi

IF=${AVB_INTERFACE:-eth0}
CONF=${AVB_GPTP_CONF:-/etc/avb/flexptpd.conf}
PRIO=${AVB_GPTP_PRIO:-53}
LOG=${AVB_GPTP_LOG:-/var/log/flexptpd.log}

RUN=/run/avb
PIDF=$RUN/flexptpd.pid

die() {
	echo "avb-gptp: $*" >&2
	exit 1
}

running() {
	[ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null
}

start() {
	[ -d "/sys/class/net/$IF" ] || die "no $IF (booted without the ethcap label?)"
	[ -r "$CONF" ] || die "no $CONF"

	mkdir -p "$RUN"

	if running; then
		echo "avb-gptp: flexptpd already running (pid $(cat "$PIDF"))"
		return 0
	fi

	# the shell execs flexptpd, so the pid file names the daemon; -P puts
	# every thread of it on SCHED_FIFO
	start-stop-daemon -S -b -m -p "$PIDF" -x /bin/sh -- -c \
		"exec /usr/sbin/flexptpd -i $IF -c $CONF -P $PRIO -q >> $LOG 2>&1"

	sleep 1

	running || die "flexptpd exited at once (see $LOG)"
	echo "avb-gptp: flexptpd pid $(cat "$PIDF") on $IF, SCHED_FIFO $PRIO"
}

stop() {
	if ! running; then
		rm -f "$PIDF"
		return 0
	fi

	start-stop-daemon -K -q -p "$PIDF" || true

	i=0
	while running && [ "$i" -lt 20 ]; do
		sleep 0.1
		i=$((i + 1))
	done

	rm -f "$PIDF"
}

# The fields that say whether the port is a synchronized gPTP slave.
status() {
	if running; then
		echo "flexptpd: running (pid $(cat "$PIDF"))"
	else
		echo "flexptpd: not running"
	fi

	milan-dp -g "flexptpd.$IF" 2>/dev/null |
		awk -F= '$1 ~ /^(port_state|as_capable|gm_identity|steps_removed|mean_link_delay_ns|time_error_ns|locked)$/ { print "  " $1 " " $2 }'
}

case "${1:-status}" in
start)   start ;;
stop)    stop ;;
restart) stop; start ;;
status)  status ;;
*) echo "usage: $0 {start|stop|restart|status}" >&2; exit 1 ;;
esac
