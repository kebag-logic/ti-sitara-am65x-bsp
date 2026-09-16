#!/bin/bash

# SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic
# SPDX-License-Identifier: MIT

# Build a recoverable full-chain spare microSD image: v2026.04 bootloaders + today's kernel/DTB/rootfs. dd to a card, boot it; pop the golden SD back to recover.
# Usage: build-spare-sd.sh [out.img]   needs: sudo, dosfstools, e2fsprogs; inputs staged under res/spare-sd/ by the session
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="${1:-$HERE/res/spare-sd/spare-am62x-v2026.img}"
# Everything below defaults to the MYIR AM6254 chain. A second board passes its
# own staged boot dir and U-Boot output in, e.g. for the TI SK-AM62B-P1:
#   BOOTSRC=res/spare-sd-sk/boot UBOUT=u-boot-official/out_sk \
#   R5_NAME=tiboot3-am62x-hs-fs-evm.bin ./build-spare-sd.sh out.img
BOOTSRC="${BOOTSRC:-$HERE/res/spare-sd/boot}"     # kernels/DTBs/extlinux staged by build-tdm8-uac2*.sh
ROOTTAR="${ROOTTAR:-$HERE/res/spare-sd/rootfs.tar.gz}"   # board rootfs; override with the Buildroot output/images/rootfs.tar.gz
UBOUT="${UBOUT:-$HERE/u-boot-official/out_myir}"
R5_NAME="${R5_NAME:-tiboot3-am62x-gp-myc-am62x.bin}"
R5="${R5:-$UBOUT/r5/$R5_NAME}"
TISPL="${TISPL:-$UBOUT/a53/tispl.bin}"
UB="${UB:-$UBOUT/a53/u-boot.img}"
# which staged kernel must be present (build-tdm8-uac2.sh stage writes Image-<rel>.gz)
KIMG_NAME="${KIMG_NAME:-Image-7.1.0.gz}"
SIZE_MB="${SIZE_MB:-1600}"; BOOT_MB="${BOOT_MB:-300}"

for f in "$BOOTSRC/$KIMG_NAME" "$ROOTTAR" "$R5" "$TISPL" "$UB"; do [ -s "$f" ] || { echo "missing input: $f"; exit 1; }; done

# losetup reports a bare "failed to set up loop device: No such file or directory"
# when the loop module cannot be loaded. The usual cause is a kernel upgrade with
# no reboot: /lib/modules/$(uname -r) is gone, so nothing can be modprobed at all.
# Check before we create a multi-GB file and repartition it.
check_loop() {
	grep -qw loop /proc/devices && return 0        # already loaded, or built in
	modprobe -qn loop 2>/dev/null && return 0      # loadable on demand
	echo "error: no usable loop device - the 'loop' module is neither loaded nor" >&2
	echo "       loadable for the running kernel $(uname -r)." >&2
	if [ ! -d "/lib/modules/$(uname -r)" ]; then
		echo "       /lib/modules/$(uname -r) does not exist; the installed module" >&2
		echo "       tree is $(ls -d /lib/modules/*/ 2>/dev/null | xargs -n1 basename | tr '\n' ' ')" >&2
		echo "       -> the kernel was upgraded and not rebooted. Reboot, then retry." >&2
	else
		echo "       -> try: sudo modprobe loop" >&2
	fi
	exit 1
}
check_loop

# The host tools these images need. mformat (mtools) is required rather than
# mkfs.vfat: see the dosfstools 4.2 alignment note further down.
check_tools() {
	miss=""
	for t in sfdisk losetup mformat mkfs.ext4; do
		command -v "$t" >/dev/null 2>&1 || miss="$miss $t"
	done
	[ -z "$miss" ] && return 0
	echo "error: missing host tool(s):$miss" >&2
	echo "       Arch: sudo pacman -S --needed mtools dosfstools e2fsprogs util-linux" >&2
	echo "       Debian/Ubuntu: sudo apt install mtools dosfstools e2fsprogs util-linux" >&2
	exit 1
}
check_tools

mkdir -p "$(dirname "$OUT")"
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
