#!/bin/sh
set -eu
repo=$(realpath "$1")
packet=$(realpath "$2")
export TMPDIR="$packet/scratch"
work="$packet/scratch/cross"
mkdir -p "$work"
cd "$work"
curl -fsSL https://www.alsa-project.org/files/pub/lib/alsa-lib-1.2.15.3.tar.bz2 -o alsa.tar.bz2
printf '%s\n' '7b079d614d582cade7ab8db2364e65271d0877a37df8757ac4ac0c8970be861e  alsa.tar.bz2' | sha256sum -c -
tar -xf alsa.tar.bz2
cd alsa-lib-1.2.15.3
./configure --host=aarch64-linux-gnu --prefix="$work/sysroot" --disable-python --disable-static
make -j16
make install
cd "$repo/milan-linux"
make -j16 MILAN_FPGA="$packet/scratch/upstream" O="$work/build" CC=aarch64-linux-gnu-gcc CFLAGS="-I$work/sysroot/include" LDFLAGS="-L$work/sysroot/lib" all
cd "$work/build"
sha256sum milan-ctrld milan-mediad milan-dp milan-gptp-watch > "$packet/cross-SHA256SUMS"
cat "$packet/cross-SHA256SUMS"
