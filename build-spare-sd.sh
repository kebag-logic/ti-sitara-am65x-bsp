#!/bin/bash

# SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic
# SPDX-License-Identifier: MIT

# Build a recoverable full-chain spare microSD image: v2026.04 bootloaders + today's kernel/DTB/rootfs. dd to a card, boot it; pop the golden SD back to recover.
# Usage: build-spare-sd.sh [out.img]   needs: sudo, dosfstools, e2fsprogs; inputs staged under res/spare-sd/ by the session
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="${1:-$HERE/res/spare-sd/spare-am62x-v2026.img}"
BOOTSRC="$HERE/res/spare-sd/boot"                 # kernels/DTBs/extlinux pulled from the working SD
ROOTTAR="$HERE/res/spare-sd/rootfs.tar.gz"        # board rootfs (bind-mount tar, no virtual fs)
R5="$HERE/u-boot-official/out_myir/r5/tiboot3-am62x-gp-myc-am62x.bin"
TISPL="$HERE/u-boot-official/out_myir/a53/tispl.bin"
UB="$HERE/u-boot-official/out_myir/a53/u-boot.img"
SIZE_MB="${SIZE_MB:-1600}"; BOOT_MB="${BOOT_MB:-300}"

for f in "$BOOTSRC/Image-7.1.0.gz" "$ROOTTAR" "$R5" "$TISPL" "$UB"; do [ -s "$f" ] || { echo "missing input: $f"; exit 1; }; done

rm -f "$OUT"; truncate -s "${SIZE_MB}M" "$OUT"
# DOS table: p1 = bootable FAT32(LBA), p2 = Linux
sfdisk "$OUT" >/dev/null <<EOF
label: dos
start=2048, size=$((BOOT_MB*2048)), type=c, bootable
type=83
EOF

LOOP=$(sudo losetup -fP --show "$OUT")
trap 'sudo umount "$MB" 2>/dev/null||true; sudo umount "$MR" 2>/dev/null||true; sudo losetup -d "$LOOP" 2>/dev/null||true' EXIT
# dosfstools 4.2 aligns the FAT, shrinking the total-sector count @0x20 (614400->614376) which the K3 boot ROM rejects (silent hang; U-Boot reads it fine). See dosfstools#165 / Bootlin. mformat keeps the full count; `mkfs.vfat -a` is the documented equivalent.
sudo mformat -R 6 -F -v BOOT -i "${LOOP}p1" ::
sudo mkfs.ext4 -q -L rootfs "${LOOP}p2"

MB=$(mktemp -d); sudo mount "${LOOP}p1" "$MB"
# new boot chain under the standard names the ROM/SPL load
sudo cp "$R5" "$MB/tiboot3.bin"; sudo cp "$TISPL" "$MB/tispl.bin"; sudo cp "$UB" "$MB/u-boot.img"
sudo cp -r "$BOOTSRC"/. "$MB"/
sudo sync; sudo umount "$MB"; rmdir "$MB"

MR=$(mktemp -d); sudo mount "${LOOP}p2" "$MR"
sudo tar -C "$MR" -xzf "$ROOTTAR"
sudo sync; sudo umount "$MR"; rmdir "$MR"
sudo losetup -d "$LOOP"; trap - EXIT

echo "built $OUT ($(du -h "$OUT" | cut -f1)); flash: sudo dd if=$OUT of=/dev/sdX bs=4M conv=fsync status=progress"
