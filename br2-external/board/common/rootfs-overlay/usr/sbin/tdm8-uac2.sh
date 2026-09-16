#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: MIT

# Bridge the 8-in/8-out TDM8 link on the board's McASP to a USB Audio Class 2.0
# gadget on the USB-C OTG port, so a host sees one 8x8 UAC2 interface.
#
# Board-independent: which McASP, which pins and which direction(s) the link
# carries all come from the device tree and /etc/tdm8/tdm8.env. Used by the
# MYIR AM6254 (McASP1 on J11, see README.tdm8-uac2.md) and the TI SK-AM62B-P1
# (McASP1, or McASP0 on the 40-pin header J3, see README.tdm8-sk-am62b.md).
#
#   FPGA 8 slots out -> McASP AXR rx -> hw:TDM8 capture  -> alsaloop -> gadget playback -> USB IN  -> host
#   host -> USB OUT  -> gadget capture -> alsaloop -> hw:TDM8 playback -> McASP AXR tx -> FPGA 8 slots in
#
# f_uac2 naming, which is the opposite of what it looks like from the board:
#   p_* = ALSA *playback* on the gadget card = USB IN  endpoint = board -> host
#   c_* = ALSA *capture*  on the gadget card = USB OUT endpoint = host -> board
# Only the OUT direction can carry an explicit feedback endpoint, which is why
# c_sync=async is what makes the host track the FPGA's oscillator.
#
# TDM8_DIRECTION trims the link to one direction. Some boards cannot do both:
# on the TI SK-AM62B-P1 the 40-pin header J3 has no McASP transmit frame sync
# pin, so k3-am625-sk-tdm8-j3.dtb is capture only and wants
# TDM8_DIRECTION=capture, which advertises an 8-in / 0-out UAC2 interface and
# runs a single alsaloop leg.
#
# usage: tdm8-uac2.sh {up|down|status|gadget-up|gadget-down|bridge-up|bridge-down}

set -e

ENV_FILE=${TDM8_ENV:-/etc/tdm8/tdm8.env}
# NB: every "[ test ] && action" below is written as an if-block or given an
# explicit "|| true" - under `set -e` a bare failing test aborts the script.
if [ -r "$ENV_FILE" ]; then . "$ENV_FILE"; fi

TDM8_RATE=${TDM8_RATE:-48000}
TDM8_CHANNELS=${TDM8_CHANNELS:-8}
TDM8_FORMAT=${TDM8_FORMAT:-S32_LE}
TDM8_DIRECTION=${TDM8_DIRECTION:-duplex}
TDM8_CARD_ID=${TDM8_CARD_ID:-TDM8}
# Boards that split the link across two McASP instances register two cards
# instead of one (e.g. k3-am625-sk-tdm8-split.dtb -> TDM8TX + TDM8RX). Both
# default to the single-card id, so a one-card board needs no change.
TDM8_CARD_ID_TX=${TDM8_CARD_ID_TX:-$TDM8_CARD_ID}
TDM8_CARD_ID_RX=${TDM8_CARD_ID_RX:-$TDM8_CARD_ID}
TDM8_GADGET_CARD_ID=${TDM8_GADGET_CARD_ID:-UAC2Gadget}
TDM8_SYNC=${TDM8_SYNC:-samplerate}
TDM8_LATENCY_US=${TDM8_LATENCY_US:-8000}
TDM8_WAIT_CARDS=${TDM8_WAIT_CARDS:-20}
TDM8_REQ_NUMBER=${TDM8_REQ_NUMBER:-4}
TDM8_FB_MAX=${TDM8_FB_MAX:-5}
TDM8_ECM=${TDM8_ECM:-yes}
TDM8_USB0_IP=${TDM8_USB0_IP:-192.168.7.10/24}
TDM8_ECM_DEV_ADDR=${TDM8_ECM_DEV_ADDR:-6a:65:62:6f:6f:00}
TDM8_ECM_HOST_ADDR=${TDM8_ECM_HOST_ADDR:-6a:65:62:6c:6f:02}

CFG=/sys/kernel/config
G=$CFG/usb_gadget/g
RUN=/run/tdm8

die() { echo "tdm8-uac2: $*" >&2; exit 1; }

# bytes per sample on the USB wire; must agree with TDM8_FORMAT
ssize_for() {
	case "$1" in
	S16_LE)  echo 2 ;;
	S24_3LE) echo 3 ;;
	S32_LE)  echo 4 ;;
	*) die "unsupported TDM8_FORMAT=$1 (use S16_LE, S24_3LE or S32_LE)" ;;
	esac
}

# resolve an ALSA card id (or any substring of its /proc/asound/cards line)
# to its numeric index; prints nothing and fails if it is not registered
card_index() {
	awk -v pat="$1" '
		/^[ ]*[0-9]+ \[/ {
			id = $0
			sub(/^[ ]*[0-9]+ \[[ ]*/, "", id)
			sub(/[ ]*\].*$/, "", id)
			if (id == pat) { exact = $1; done = 1; exit }
			if (fuzzy == "" && index($0, pat) > 0) fuzzy = $1
		}
		END {
			if (done)          { print exact; exit 0 }
			if (fuzzy != "")   { print fuzzy; exit 0 }
			exit 1
		}
	' /proc/asound/cards
}

wait_for_card() {
	i=0
	while [ "$i" -lt "$TDM8_WAIT_CARDS" ]; do
		card_index "$1" && return 0
		i=$((i + 1))
		sleep 1
	done
	return 1
}

# ---------------------------------------------------------------- gadget ----

gadget_down() {
	[ -d "$G" ] || return 0
	echo "" > "$G/UDC" 2>/dev/null || true
	for l in "$G"/configs/*/*; do [ -L "$l" ] && rm -f "$l"; done || true
	rmdir "$G"/configs/*/strings/* "$G"/configs/* "$G"/functions/* \
	      "$G"/strings/* "$G" 2>/dev/null || true
}

gadget_up() {
	# validate the format first: a typo here is cheaper to report before we
	# have taken the UDC away from whatever gadget currently owns it
	ssize=$(ssize_for "$TDM8_FORMAT")
	chmask=$(printf '0x%x' $(((1 << TDM8_CHANNELS) - 1)))
	# f_uac2 drops a direction entirely when its channel mask is 0, which is
	# how a capture-only board still presents a valid UAC2 interface.
	case "$TDM8_DIRECTION" in
	duplex)   p_chmask=$chmask; c_chmask=$chmask ;;
	capture)  p_chmask=$chmask; c_chmask=0 ;;
	playback) p_chmask=0;       c_chmask=$chmask ;;
	*) die "unsupported TDM8_DIRECTION=$TDM8_DIRECTION (use duplex, capture or playback)" ;;
	esac

	modprobe libcomposite 2>/dev/null || true
	grep -q " $CFG configfs" /proc/mounts || mount -t configfs none "$CFG"

	# there is exactly one DRD controller, so take it back from any other gadget
	for o in "$CFG"/usb_gadget/*; do
		[ -d "$o" ] || continue
		echo "" > "$o/UDC" 2>/dev/null || true
	done
	gadget_down

	udc=${TDM8_UDC:-$(ls /sys/class/udc 2>/dev/null | head -1)}
	[ -n "$udc" ] || die "no UDC in /sys/class/udc (is the DWC3 in peripheral mode?)"

	mkdir -p "$G"
	echo 0x1d6b > "$G/idVendor"		# Linux Foundation
	echo 0x0104 > "$G/idProduct"		# Multifunction Composite Gadget
	echo 0x0100 > "$G/bcdDevice"
	echo 0x0200 > "$G/bcdUSB"		# USB 2.0: this board has no USB3 PHY
	if [ "$TDM8_ECM" = yes ]; then
		# Miscellaneous / Common / Interface Association Descriptor: the
		# correct triple for a composite device built from IAD functions
		echo 0xef > "$G/bDeviceClass"
		echo 0x02 > "$G/bDeviceSubClass"
		echo 0x01 > "$G/bDeviceProtocol"
	fi

	mkdir -p "$G/strings/0x409"
	echo "Kebag-Logic"                            > "$G/strings/0x409/manufacturer"
	echo "AM62x TDM8 ${TDM8_CHANNELS}x${TDM8_CHANNELS} UAC2" > "$G/strings/0x409/product"
	echo "0001"                                   > "$G/strings/0x409/serialnumber"

	f=$G/functions/uac2.0
	mkdir -p "$f"
	echo "$p_chmask"  > "$f/p_chmask"	# board -> host (USB IN)
	echo "$c_chmask"  > "$f/c_chmask"	# host -> board (USB OUT)
	echo "$TDM8_RATE" > "$f/p_srate"
	echo "$TDM8_RATE" > "$f/c_srate"
	echo "$ssize"     > "$f/p_ssize"
	echo "$ssize"     > "$f/c_ssize"
	# explicit feedback on the OUT endpoint: the host rate-adapts to the FPGA
	if [ -e "$f/c_sync" ];     then echo async              > "$f/c_sync";     fi
	if [ -e "$f/fb_max" ];     then echo "$TDM8_FB_MAX"     > "$f/fb_max";     fi
	if [ -e "$f/req_number" ]; then echo "$TDM8_REQ_NUMBER" > "$f/req_number"; fi
	if [ -e "$f/function_name" ]; then
		echo "KL TDM8 ${TDM8_CHANNELS}x${TDM8_CHANNELS}" > "$f/function_name"
	fi

	if [ "$TDM8_ECM" = yes ]; then
		mkdir -p "$G/functions/ecm.usb0"
		echo "$TDM8_ECM_DEV_ADDR"  > "$G/functions/ecm.usb0/dev_addr"
		echo "$TDM8_ECM_HOST_ADDR" > "$G/functions/ecm.usb0/host_addr"
	fi

	mkdir -p "$G/configs/c.1/strings/0x409"
	echo "TDM8 ${TDM8_CHANNELS}ch @ ${TDM8_RATE} Hz" > "$G/configs/c.1/strings/0x409/configuration"
	echo 250 > "$G/configs/c.1/MaxPower"
	ln -sf "$f" "$G/configs/c.1/"
	if [ "$TDM8_ECM" = yes ]; then ln -sf "$G/functions/ecm.usb0" "$G/configs/c.1/"; fi

	if command -v udevadm >/dev/null 2>&1; then udevadm settle -t 5 || true; fi
	echo "$udc" > "$G/UDC"

	if [ "$TDM8_ECM" = yes ] && [ -n "$TDM8_USB0_IP" ]; then
		ip addr add "$TDM8_USB0_IP" dev usb0 2>/dev/null || true
		ip link set usb0 up || true
	fi

	echo "tdm8-uac2: gadget bound to $udc" \
	     "(p_chmask=$p_chmask c_chmask=$c_chmask, dir=$TDM8_DIRECTION," \
	     "${TDM8_RATE} Hz, ${ssize}-byte samples, OUT sync=async)"
}

# ---------------------------------------------------------------- bridge ----

loop_start() { # <tag> <capture dev> <playback dev>
	tag=$1; capt=$2; play=$3
	command -v alsaloop >/dev/null 2>&1 \
		|| die "alsaloop is missing (BR2_PACKAGE_ALSA_UTILS_ALSALOOP)"
	alsaloop -C "$capt" -P "$play" \
		 -c "$TDM8_CHANNELS" -r "$TDM8_RATE" -f "$TDM8_FORMAT" \
		 -t "$TDM8_LATENCY_US" -S "$TDM8_SYNC" -z \
		 >/dev/null 2>&1 &
	echo $! > "$RUN/$tag.pid"
	echo "tdm8-uac2: $tag $capt -> $play (pid $!)"
}

loop_stop() {
	for p in "$RUN"/*.pid; do
		[ -f "$p" ] || continue
		pid=$(cat "$p")
		kill "$pid" 2>/dev/null || true
		rm -f "$p"
	done
}

bridge_up() {
	mkdir -p "$RUN"
	loop_stop

	# eudev normally modprobes these off the DT modalias; do it explicitly
	# too so a missing uevent does not turn into a 20-second timeout
	for m in snd-soc-davinci-mcasp snd-soc-kl-tdm8-dummy snd-soc-simple-card; do
		modprobe "$m" 2>/dev/null || true
	done

	# only wait for the card(s) the enabled direction(s) actually need
	case "$TDM8_DIRECTION" in
	duplex)   need="$TDM8_CARD_ID_RX $TDM8_CARD_ID_TX" ;;
	capture)  need="$TDM8_CARD_ID_RX" ;;
	playback) need="$TDM8_CARD_ID_TX" ;;
	*)        die "unsupported TDM8_DIRECTION=$TDM8_DIRECTION" ;;
	esac
	for c in $need; do
		wait_for_card "$c" >/dev/null \
			|| die "ALSA card '$c' never appeared - is the McASP node enabled and snd-soc-kl-tdm8-dummy loaded?"
	done
	wait_for_card "$TDM8_GADGET_CARD_ID" >/dev/null \
		|| die "ALSA card '$TDM8_GADGET_CARD_ID' never appeared - the UAC2 gadget is not bound"

	tdm_rx=$(card_index "$TDM8_CARD_ID_RX" 2>/dev/null || true)
	tdm_tx=$(card_index "$TDM8_CARD_ID_TX" 2>/dev/null || true)
	gad=$(card_index "$TDM8_GADGET_CARD_ID")

	# The FPGA is the clock master: with no BCLK/FSYNC on the link the McASP
	# never advances and the loops stall. Bring the FPGA up before this runs.
	case "$TDM8_DIRECTION" in
	duplex)
		loop_start to-host   "hw:$tdm_rx,0" "hw:$gad,0"
		loop_start from-host "hw:$gad,0"    "hw:$tdm_tx,0"
		;;
	capture)
		loop_start to-host   "hw:$tdm_rx,0" "hw:$gad,0"
		;;
	playback)
		loop_start from-host "hw:$gad,0"    "hw:$tdm_tx,0"
		;;
	*)
		die "unsupported TDM8_DIRECTION=$TDM8_DIRECTION"
		;;
	esac
}

bridge_down() { loop_stop; }

# ---------------------------------------------------------------- status ----

status() {
	echo "--- ALSA cards ---"
	cat /proc/asound/cards 2>/dev/null || echo "(no sound cards)"
	echo "--- gadget ---"
	if [ -d "$G" ]; then
		echo "UDC:        $(cat "$G/UDC" 2>/dev/null)"
		echo "functions:  $(ls "$G/functions" 2>/dev/null | tr '\n' ' ')"
		for a in p_chmask c_chmask p_srate c_srate p_ssize c_ssize c_sync fb_max req_number; do
			if [ -e "$G/functions/uac2.0/$a" ]; then
				echo "uac2.$a = $(cat "$G/functions/uac2.0/$a")"
			fi
		done
	else
		echo "(no gadget at $G)"
	fi
	echo "--- bridge ---"
	for p in "$RUN"/*.pid; do
		[ -f "$p" ] || continue
		pid=$(cat "$p")
		if kill -0 "$pid" 2>/dev/null; then
			echo "$(basename "$p" .pid): running (pid $pid)"
		else
			echo "$(basename "$p" .pid): DEAD"
		fi
	done
	echo "--- PCM state ---"
	for s in /proc/asound/card*/pcm*/sub*/status; do
		[ -f "$s" ] || continue
		echo "== $s"
		cat "$s"
	done
}

case "${1:-status}" in
up)          gadget_up; bridge_up ;;
down)        bridge_down; gadget_down ;;
gadget-up)   gadget_up ;;
gadget-down) gadget_down ;;
bridge-up)   bridge_up ;;
bridge-down) bridge_down ;;
status)      status ;;
*) echo "usage: $0 {up|down|status|gadget-up|gadget-down|bridge-up|bridge-down}" >&2; exit 1 ;;
esac
