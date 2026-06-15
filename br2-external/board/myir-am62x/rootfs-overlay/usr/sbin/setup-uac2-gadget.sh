#!/bin/bash

# SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic
# SPDX-License-Identifier: MIT

# Bring the AM62x USB DRD up as a USB Audio Class 2.0 (UAC2) gadget at USB 2.0 high-speed
# Usage: setup-uac2-gadget.sh [up|down]   env overrides: SRATE CHMASK SSIZE GNAME UDC
set -e
ACTION="${1:-up}"
CFG=/sys/kernel/config
GNAME="${GNAME:-uac2}"; G="$CFG/usb_gadget/$GNAME"
SRATE="${SRATE:-48000}"   # sample rate (matches the 48k AVB AAF stream)
CHMASK="${CHMASK:-3}"     # channel mask, 3 = stereo (ch 1+2)
SSIZE="${SSIZE:-2}"       # bytes per sample, 2 = 16-bit

# configfs teardown must drop function links first, then dirs, in reverse order
teardown() {
	[ -d "$G" ] || return 0
	echo "" > "$G/UDC" 2>/dev/null || true
	for l in "$G"/configs/*/*; do [ -L "$l" ] && rm -f "$l"; done
	rmdir "$G"/configs/*/strings/* "$G"/configs/* "$G"/functions/* "$G"/strings/* "$G" 2>/dev/null || true
}

modprobe libcomposite 2>/dev/null || true
grep -q " $CFG configfs" /proc/mounts || mount -t configfs none "$CFG"
[ "$ACTION" = "down" ] && { teardown; echo "uac2 gadget removed"; exit 0; }

# only one DRD UDC exists, so release it from any other gadget before binding ours
UDC="${UDC:-$(ls /sys/class/udc | head -1)}"
for o in "$CFG"/usb_gadget/*; do [ "$o" = "$G" ] || echo "" > "$o/UDC" 2>/dev/null || true; done
teardown

mkdir -p "$G"
echo 0x1d6b > "$G/idVendor"    # Linux Foundation
echo 0x0104 > "$G/idProduct"   # Multifunction Composite Gadget
echo 0x0100 > "$G/bcdDevice"
echo 0x0200 > "$G/bcdUSB"      # USB 2.0 (high-speed)
mkdir -p "$G/strings/0x409"
echo "Kebag-Logic"    > "$G/strings/0x409/manufacturer"
echo "AM62x AVB UAC2" > "$G/strings/0x409/product"
echo "0001"           > "$G/strings/0x409/serialnumber"

# p_* = playback (host->board, ALSA capture side); c_* = capture (board->host, ALSA playback side)
mkdir -p "$G/functions/uac2.0"
echo "$SRATE"  > "$G/functions/uac2.0/p_srate"
echo "$SRATE"  > "$G/functions/uac2.0/c_srate"
echo "$CHMASK" > "$G/functions/uac2.0/p_chmask"
echo "$CHMASK" > "$G/functions/uac2.0/c_chmask"
echo "$SSIZE"  > "$G/functions/uac2.0/p_ssize"
echo "$SSIZE"  > "$G/functions/uac2.0/c_ssize"

mkdir -p "$G/configs/c.1/strings/0x409"
echo "UAC2 ${SRATE}Hz" > "$G/configs/c.1/strings/0x409/configuration"
echo 250 > "$G/configs/c.1/MaxPower"
ln -sf "$G/functions/uac2.0" "$G/configs/c.1/"

echo "$UDC" > "$G/UDC"
echo "UAC2 gadget '$GNAME' bound: UDC=$UDC USB2.0 srate=${SRATE} chmask=${CHMASK} ssize=${SSIZE}"
