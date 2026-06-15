#!/bin/bash

# SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic
# SPDX-License-Identifier: MIT

# Recoverably flash the v2026.04 U-Boot to the MYIR AM62x SD boot partition: backup+stage keep the SD bootable; only 'activate' swaps the live blobs (rollback restores them)
# Usage: flash-uboot-sd.sh {backup|stage|verify|activate|rollback|status}   env: BOARD MNT
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
BOARD="${BOARD:-board}"          # ssh alias (ProxyJump via jump-host)
MNT="${MNT:-/mnt/bootp}"             # board mountpoint for /dev/mmcblk1p1
PART="${PART:-/dev/mmcblk1p1}"
HOSTBK="$HERE/res/sd-boot-backup"    # off-board copy of the working blobs
R5="$HERE/u-boot-official/out_myir/r5"
A53="$HERE/u-boot-official/out_myir/a53"
# live SD name  ::  freshly-built source artifact
MAP="tiboot3.bin::$R5/tiboot3-am62x-gp-myc-am62x.bin tispl.bin::$A53/tispl.bin u-boot.img::$A53/u-boot.img"

rmount() { ssh "$BOARD" "mkdir -p $MNT; mountpoint -q $MNT || mount $PART $MNT"; }
rumount() { ssh "$BOARD" "sync; umount $MNT 2>/dev/null || true"; }

backup() {
	# Copy the live blobs to an on-SD boot-backup/ AND pull them to the dev host (two independent restore paths)
	mkdir -p "$HOSTBK"
	rmount
	ssh "$BOARD" "set -e; mkdir -p $MNT/boot-backup
		for f in tiboot3.bin tispl.bin u-boot.img; do [ -f $MNT/boot-backup/\$f ] || cp $MNT/\$f $MNT/boot-backup/\$f; done
		md5sum $MNT/boot-backup/* > $MNT/boot-backup/MD5SUMS; cat $MNT/boot-backup/MD5SUMS"
	for f in tiboot3.bin tispl.bin u-boot.img; do scp "$BOARD:$MNT/boot-backup/$f" "$HOSTBK/$f"; done
	rumount
	echo "backup: on-SD $MNT/boot-backup/ + host $HOSTBK/"
}

stage() {
	# Push the new blobs to the SD under *.v2026 names (inert: the ROM/SPL still load the live names)
	rmount
	for m in $MAP; do n="${m%%::*}"; s="${m##*::}"; scp "$s" "$BOARD:$MNT/$n.v2026"; done
	ssh "$BOARD" "sync; md5sum $MNT/*.v2026"
	rumount
	echo "staged *.v2026 on SD (not yet active)"
}

verify() {
	# Confirm staged *.v2026 match the host artifacts byte-for-byte
	rmount
	for m in $MAP; do n="${m%%::*}"; s="${m##*::}"
		h=$(md5sum "$s" | cut -d' ' -f1); b=$(ssh "$BOARD" "md5sum $MNT/$n.v2026 2>/dev/null | cut -d' ' -f1")
		[ "$h" = "$b" ] && echo "OK   $n.v2026 ($h)" || echo "DIFF $n.v2026 host=$h board=$b"
	done
	rumount
}

status() {
	rmount
	ssh "$BOARD" "ls -l $MNT/tiboot3.bin $MNT/tispl.bin $MNT/u-boot.img $MNT/*.v2026 $MNT/boot-backup/* 2>/dev/null"
	rumount
}

activate() {
	# IRREVERSIBLE on next boot: overwrite the live blobs with *.v2026. Do this only at the serial console with an SD reader on hand.
	backup
	rmount
	ssh "$BOARD" "set -e; for f in tiboot3.bin tispl.bin u-boot.img; do cp $MNT/\$f.v2026 $MNT/\$f; done; sync; md5sum $MNT/tiboot3.bin $MNT/tispl.bin $MNT/u-boot.img"
	rumount
	echo "ACTIVATED v2026.04 — reboot to test; if it fails, recover at serial then './flash-uboot-sd.sh rollback'"
}

rollback() {
	# Restore the working blobs from the on-SD backup
	rmount
	ssh "$BOARD" "set -e; for f in tiboot3.bin tispl.bin u-boot.img; do cp $MNT/boot-backup/\$f $MNT/\$f; done; sync; md5sum $MNT/tiboot3.bin $MNT/tispl.bin $MNT/u-boot.img"
	rumount
	echo "rolled back to the pre-flash blobs"
}

case "${1:-status}" in
	backup) backup ;;
	stage) stage ;;
	verify) verify ;;
	status) status ;;
	activate) activate ;;
	rollback) rollback ;;
	*) echo "Usage: $0 {backup|stage|verify|activate|rollback|status}"; exit 1 ;;
esac
