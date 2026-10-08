#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0

# gPTP (802.1AS) on the PocketBeagle 2 Ethernet Cap (issue #4): ptp4l on the
# CPTS hardware clock of eth0, and phc2sys steering CLOCK_REALTIME from it.
#
# ptp4l exports the read-only management socket /var/run/ptp4lro (gPTP.cfg),
# which the bridge reads for the grandmaster and asCapable. Both daemons log to
# syslog. Management queries need transportSpecific 1, so pmc runs with -t 1.
#
# usage: avb-gptp.sh {start|stop|restart|status}

set -e

ENV_FILE=${AVB_ENV:-/etc/avb/avb.env}
if [ -r "$ENV_FILE" ]; then . "$ENV_FILE"; fi

IF=${AVB_INTERFACE:-eth0}
CFG=${AVB_GPTP_CFG:-/etc/avb/gPTP.cfg}
RUN=/run/avb
PMC="pmc -u -b 0 -t 1"

die() { echo "avb-gptp: $*" >&2; exit 1; }

daemon_start() { # <name> <rt prio> <args...>
	name=$1; prio=$2; shift 2
	pidf="$RUN/$name.pid"
	if [ -f "$pidf" ] && kill -0 "$(cat "$pidf")" 2>/dev/null; then
		echo "avb-gptp: $name already running (pid $(cat "$pidf"))"
		return 0
	fi
	start-stop-daemon -S -b -m -p "$pidf" -x "/usr/sbin/$name" -- "$@"
	sleep 1
	pid=$(cat "$pidf")
	kill -0 "$pid" 2>/dev/null || die "$name exited at once (see /var/log/messages)"
	chrt -f -p "$prio" "$pid" >/dev/null
	echo "avb-gptp: $name pid $pid, SCHED_FIFO $prio"
}

daemon_stop() { # <name>
	pidf="$RUN/$1.pid"
	if [ -f "$pidf" ]; then
		start-stop-daemon -K -q -p "$pidf" || true
		rm -f "$pidf"
	fi
}

start() {
	[ -d "/sys/class/net/$IF" ] || die "no $IF (booted without the ethcap label?)"
	[ -r "$CFG" ] || die "no $CFG"
	mkdir -p "$RUN"
	daemon_start ptp4l "${AVB_PTP4L_PRIO:-53}" -f "$CFG" -i "$IF"
	# -w waits for ptp4l to lock and takes the UTC offset from it
	daemon_start phc2sys "${AVB_PHC2SYS_PRIO:-52}" -s "$IF" -c CLOCK_REALTIME -w \
		--transportSpecific=1 -S 1.0
}

stop() {
	daemon_stop phc2sys
	daemon_stop ptp4l
}

# One line per field, as pmc prints them, from the three data sets that say
# whether the port is a synchronized gPTP slave.
status() {
	for n in ptp4l phc2sys; do
		if [ -f "$RUN/$n.pid" ] && kill -0 "$(cat "$RUN/$n.pid")" 2>/dev/null; then
			echo "$n: running (pid $(cat "$RUN/$n.pid"))"
		else
			echo "$n: not running"
		fi
	done
	$PMC 'GET PORT_DATA_SET' 'GET TIME_STATUS_NP' 'GET PARENT_DATA_SET' 2>/dev/null |
		awk '$1 ~ /^(portState|peerMeanPathDelay|master_offset|gmPresent|gmIdentity|grandmasterIdentity|parentPortIdentity)$/ { print "  " $1 " " $2 }'
}

case "${1:-status}" in
start)   start ;;
stop)    stop ;;
restart) stop; start ;;
status)  status ;;
*) echo "usage: $0 {start|stop|restart|status}" >&2; exit 1 ;;
esac
