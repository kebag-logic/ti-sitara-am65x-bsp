#!/bin/bash

# SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic
# SPDX-License-Identifier: MIT

# Build a recoverable A/B spare microSD (safe-update P2a): GPT with boot + rootfs.A + rootfs.B, each slot carrying its own kernel in /boot, driven by the RAUC-compatible U-Boot bootchooser (res/ab/boot.cmd -> boot.scr) using the P0 persistent env. dd to a card, boot, validate slot-switch/rollback.
# Usage: build-spare-sd-ab.sh [out.img]   needs sudo, dosfstools, e2fsprogs, mkimage
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="${1:-$HERE/res/spare-sd/spare-am62x-ab.img}"
R5="$HERE/u-boot-official/out_myir/r5/tiboot3-am62x-gp-myc-am62x.bin"
TISPL="$HERE/u-boot-official/out_myir/a53/tispl.bin"
UB="$HERE/u-boot-official/out_myir/a53/u-boot.img"
MKIMAGE="$HERE/u-boot-official/out_myir/a53/tools/mkimage"
KIMG="$HERE/linux/arch/arm64/boot/Image"
DTB="$HERE/res/spare-sd/boot/ti/k3-am625x-myd-6254-71.dtb"
ROOTTAR="$HERE/res/spare-sd/rootfs.tar.gz"
SIZE_MB="${SIZE_MB:-3000}"; BOOT_MB="${BOOT_MB:-300}"; SLOT_MB="${SLOT_MB:-1300}"

for f in "$R5" "$TISPL" "$UB" "$MKIMAGE" "$KIMG" "$DTB" "$ROOTTAR" "$HERE/res/ab/boot.cmd"; do [ -s "$f" ] || { echo "missing input: $f"; exit 1; }; done
"$MKIMAGE" -A arm64 -T script -C none -d "$HERE/res/ab/boot.cmd" "$HERE/res/ab/boot.scr" >/dev/null

rm -f "$OUT"; truncate -s "${SIZE_MB}M" "$OUT"
# GPT: p1 bootable FAT32 (boot), p2 rootfs.A, p3 rootfs.B
sfdisk "$OUT" >/dev/null <<EOF
label: gpt
start=2048, size=$((BOOT_MB*2048)), type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name=boot
size=$((SLOT_MB*2048)), type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name=rootfs.A
type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name=rootfs.B
EOF

LOOP=$(sudo losetup -fP --show "$OUT")
trap 'sudo umount "$MB" "$MA" "$MBp" 2>/dev/null||true; sudo losetup -d "$LOOP" 2>/dev/null||true' EXIT
sudo mkfs.vfat -F 32 -n boot "${LOOP}p1" >/dev/null
sudo mkfs.ext4 -q -L rootfs.A "${LOOP}p2"; sudo mkfs.ext4 -q -L rootfs.B "${LOOP}p3"

# boot partition: bootloaders + bootchooser (NO extlinux.conf, so distro_bootcmd runs boot.scr)
MB=$(mktemp -d); sudo mount "${LOOP}p1" "$MB"
sudo cp "$R5" "$MB/tiboot3.bin"; sudo cp "$TISPL" "$MB/tispl.bin"; sudo cp "$UB" "$MB/u-boot.img"
sudo cp "$HERE/res/ab/boot.scr" "$MB/boot.scr"
sudo sync; sudo umount "$MB"; rmdir "$MB"

# both slots: identical rootfs + the slot's own kernel/dtb in /boot
MA=$(mktemp -d); MBp=$(mktemp -d)
sudo mount "${LOOP}p2" "$MA"; sudo mount "${LOOP}p3" "$MBp"
for m in "$MA" "$MBp"; do
	sudo tar -C "$m" -xzf "$ROOTTAR"
	sudo mkdir -p "$m/boot"
	sudo cp "$KIMG" "$m/boot/Image"; sudo cp "$DTB" "$m/boot/k3-am625x-myd-6254-71.dtb"
done
sudo sync; sudo umount "$MA" "$MBp"; rmdir "$MA" "$MBp"
sudo losetup -d "$LOOP"; trap - EXIT

echo "built $OUT ($(du -h "$OUT"|cut -f1)); flash: sudo dd if=$OUT of=/dev/sdX bs=4M conv=fsync status=progress"
