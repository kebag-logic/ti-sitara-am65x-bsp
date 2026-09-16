#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: MIT

# Let PocketBeagle 2's U-Boot boot a gzipped kernel.
#
# The symptom is at the very end of an otherwise perfect boot: the extlinux
# menu appears, the label is chosen, the Image is retrieved, and then
#
#   Retrieving file: /Image-7.1.0-tdm8-pb2.gz
#   kernel_comp_addr_r or kernel_comp_size is not provided!
#   Boot failed (err=-14)
#
# for every label in the menu, followed by "No more bootdevs".
#
# cmd/booti.c sniffs the image, sees gzip, and needs somewhere to decompress
# to.  Both variables must be set or it refuses:
#
#   dest     = env_get_ulong("kernel_comp_addr_r", 16, 0);  <- destination
#   comp_len = env_get_ulong("kernel_comp_size",   16, 0);  <- compressed bound
#   decomp_len = comp_len * 10;                             <- max output
#   ... image_decomp(...) ; memmove((void *)ld, (void *)dest, dest_end);
#
# so the decompressed kernel is built at kernel_comp_addr_r and then moved back
# to the load address.  board/beagle/pocketbeagle2/pocketbeagle2.env does not
# set either one; env/ti/ti_common.env only gives loadaddr/kernel_addr_r
# (0x82000000) and fdtaddr (0x88000000).
#
# The values below are the ones res/uEnv.txt already uses on the MYIR board,
# and they fit PocketBeagle 2's 512 MB (0x80000000-0xa0000000):
#
#   kernel_comp_size   0x2000000  32 MB, only has to exceed the COMPRESSED
#                                 image (~15 MB); 10x that caps the output
#   kernel_comp_addr_r 0x90000000 scratch for the ~41 MB decompressed Image.
#                                 Clear of kernel_addr_r (0x82000000 + 15 MB),
#                                 of fdtaddr (0x88000000), of where the kernel
#                                 lands after the memmove (0x82000000 + 41 MB
#                                 = 0x84940000), and below the reserved
#                                 wkup-R5 / TF-A / OP-TEE carveouts that start
#                                 at 0x9da00000.
#
# The alternative is to stage an uncompressed Image, which booti takes without
# any of this - at 41 MB on the FAT partition instead of 15 MB.
#
# Only u-boot.img changes; tiboot3.bin and tispl.bin are untouched.
#
# Idempotent.  Usage: fix-pb2-kernel-comp.sh [<u-boot-src>]  (default: ../../u-boot-pb)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
UB=${1:-$(cd "$HERE/../.." && pwd)/u-boot-pb}
E="$UB/board/beagle/pocketbeagle2/pocketbeagle2.env"

[ -f "$UB/Makefile" ] || { echo "not a U-Boot tree: $UB" >&2; exit 1; }
[ -f "$E" ] || { echo "no $E - not a PocketBeagle 2 U-Boot tree" >&2; exit 1; }

if grep -q '^kernel_comp_addr_r=' "$E"; then
	echo "kernel_comp: already set in $(basename "$E")"
	exit 0
fi

cat >> "$E" <<'ENV'

# Required by cmd/booti.c to boot a gzipped Image; without both of these it
# prints "kernel_comp_addr_r or kernel_comp_size is not provided!" and every
# extlinux label fails with err=-14. 0x90000000 is scratch for the ~41 MB
# decompressed kernel, clear of loadaddr/fdtaddr and of the reserved carveouts
# above 0x9da00000. kernel_comp_size only has to exceed the compressed size.
kernel_comp_addr_r=0x90000000
kernel_comp_size=0x2000000
ENV

echo "kernel_comp: appended to $E"
echo "rebuild the A53 half; only u-boot.img needs to go back on the card"
