#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: MIT

# AVB egress on the PocketBeagle 2 Ethernet Cap (issue #5): the VLAN of the
# streams, the socket-priority to PCP map, and the am65-cpsw class A/B shaper.
#
# am65-cpsw has no CBS qdisc offload. It shapes in hardware through mqprio in
# channel mode with "shaper bw_rlimit": each shaped traffic class gets its own
# host TX DMA channel and a committed (min_rate) and excess (max_rate) rate on
# the port, in whole Mbit/s. Two things must hold first, and both can change
# only while every CPSW port is down:
#   - the private flag p0-rx-ptype-rrobin is off. With it on (the driver's
#     default) the host port serves its TX channels round robin and sends all
#     host traffic to port FIFO 0, so no traffic class is ever shaped;
#   - the number of TX channels equals the number of traffic classes.
# So "start" takes the interface down and up once when either is wrong. eth0 is
# also the management link: run this from usb0 or the serial console, or expect
# an ssh session over eth0 to stall for the few seconds autonegotiation takes.
#
#   skb priority AVB_CLASS_A_PRIO -> TC1 (or TC2 with class B) -> PCP 3 on the VLAN
#   skb priority AVB_CLASS_B_PRIO -> TC1 (class B only)        -> PCP 2 on the VLAN
#   everything else               -> TC0, unshaped, untagged management traffic
#
# usage: avb-shaper.sh {start|stop|status}

set -e

ENV_FILE=${AVB_ENV:-/etc/avb/avb.env}
if [ -r "$ENV_FILE" ]; then . "$ENV_FILE"; fi

IF=${AVB_INTERFACE:-eth0}
VID=${AVB_VLAN_ID:-2}
VIF="$IF.$VID"
A_PRIO=${AVB_CLASS_A_PRIO:-3}
B_PRIO=${AVB_CLASS_B_PRIO:-2}
B_MBIT=${AVB_CLASS_B_MBIT:-0}

die() { echo "avb-shaper: $*" >&2; exit 1; }

# Class A bandwidth in whole Mbit/s, rounded up. One frame on the wire is the
# 8-byte preamble and SFD, the 18-byte tagged Ethernet header, the 24-byte AAF
# header, the payload, the 4-byte FCS and the 12-byte gap: 66 bytes + payload.
class_a_mbit() {
	if [ -n "$AVB_CLASS_A_MBIT" ]; then echo "$AVB_CLASS_A_MBIT"; return; fi
	payload=$(( ${AVB_CLASS_A_SAMPLES_PER_FRAME:-6} * ${AVB_CLASS_A_CHANNELS:-8} * \
		${AVB_CLASS_A_SAMPLE_BYTES:-4} ))
	bps=$(( ${AVB_CLASS_A_STREAMS:-1} * ${AVB_CLASS_A_FRAMES_PER_SEC:-8000} * \
		(66 + payload) * 8 ))
	echo $(( (bps + 999999) / 1000000 ))
}

priv_rrobin() {
	ethtool --show-priv-flags "$IF" 2>/dev/null |
		awk -F: '/p0-rx-ptype-rrobin/ { gsub(/[ \t]/, "", $2); print $2 }'
}

tx_channels() {
	ethtool -l "$IF" 2>/dev/null |
		awk '/^Current hardware settings/ { c = 1 } c && /^TX:/ { print $2; exit }'
}

# The traffic-class layout: "num_tc map queues min_rate max_rate", one line.
layout() {
	a=$(class_a_mbit)
	[ "$a" -gt 0 ] || die "class A bandwidth is 0 Mbit/s"
	map=""
	if [ "$B_MBIT" -gt 0 ]; then
		for p in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
			if [ "$p" -eq "$A_PRIO" ]; then t=2; elif [ "$p" -eq "$B_PRIO" ]; then t=1; else t=0; fi
			map="$map $t"
		done
		echo "3|$map|1@0 1@1 1@2|0 ${B_MBIT}mbit ${a}mbit|0 ${B_MBIT}mbit ${a}mbit"
	else
		for p in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
			if [ "$p" -eq "$A_PRIO" ]; then t=1; else t=0; fi
			map="$map $t"
		done
		echo "2|$map|1@0 1@1|0 ${a}mbit|0 ${a}mbit"
	fi
}

link_prepare() { # <tx channels>
	ntx=$1
	rr=$(priv_rrobin)
	cur=$(tx_channels)
	[ -n "$rr" ] || die "$IF has no p0-rx-ptype-rrobin flag: not an am65-cpsw port?"
	if [ "$rr" = off ] && [ "$cur" = "$ntx" ]; then return 0; fi
	echo "avb-shaper: $IF down/up: p0-rx-ptype-rrobin $rr -> off, TX channels $cur -> $ntx"
	ip link set "$IF" down
	ethtool --set-priv-flags "$IF" p0-rx-ptype-rrobin off ||
		{ ip link set "$IF" up; die "p0-rx-ptype-rrobin refused (another CPSW port up?)"; }
	ethtool -L "$IF" tx "$ntx" || { ip link set "$IF" up; die "ethtool -L $IF tx $ntx refused"; }
	ip link set "$IF" up
}

vlan_up() {
	if [ ! -d "/sys/class/net/$VIF" ]; then
		ip link add link "$IF" name "$VIF" type vlan id "$VID" \
			egress-qos-map "$A_PRIO:$A_PRIO" "$B_PRIO:$B_PRIO"
	else
		ip link set "$VIF" type vlan egress-qos-map "$A_PRIO:$A_PRIO" "$B_PRIO:$B_PRIO"
	fi
	# raw L2 only: no address, and no IPv6 link-local chatter on the stream VLAN
	{ echo 1 > "/proc/sys/net/ipv6/conf/$VIF/disable_ipv6"; } 2>/dev/null || true
	ip link set "$VIF" up
}

start() {
	[ -d "/sys/class/net/$IF" ] || die "no $IF (booted without the ethcap label?)"
	l=$(layout)
	ntc=${l%%|*}; r=${l#*|}
	map=${r%%|*}; r=${r#*|}
	queues=${r%%|*}; r=${r#*|}
	minr=${r%%|*}; maxr=${r#*|}
	link_prepare "$ntc"
	vlan_up
	tc qdisc del dev "$IF" root 2>/dev/null || true
	# shellcheck disable=SC2086 # the lists are meant to split
	tc qdisc replace dev "$IF" root handle 100: mqprio num_tc "$ntc" map $map \
		queues $queues hw 1 mode channel shaper bw_rlimit \
		min_rate $minr max_rate $maxr
	echo "avb-shaper: $IF num_tc $ntc, class A $(class_a_mbit) Mbit/s (prio $A_PRIO)," \
	     "class B $B_MBIT Mbit/s, VLAN $VID on $VIF"
}

stop() {
	tc qdisc del dev "$IF" root 2>/dev/null || true
	if [ -d "/sys/class/net/$VIF" ]; then ip link del "$VIF"; fi
	echo "avb-shaper: shaper and $VIF removed (p0-rx-ptype-rrobin and TX channels left as they are)"
}

status() {
	echo "class A reservation: $(class_a_mbit) Mbit/s, class B: $B_MBIT Mbit/s"
	echo "p0-rx-ptype-rrobin: $(priv_rrobin)  TX channels: $(tx_channels)"
	tc -s qdisc show dev "$IF"
	if [ -d "/sys/class/net/$VIF" ]; then ip -d link show "$VIF"; else echo "no $VIF"; fi
}

case "${1:-status}" in
start)  start ;;
stop)   stop ;;
status) status ;;
*) echo "usage: $0 {start|stop|status}" >&2; exit 1 ;;
esac
