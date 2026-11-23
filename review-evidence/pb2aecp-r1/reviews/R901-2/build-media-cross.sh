#!/bin/sh
set -eu
REPO=$(realpath "$1")
PACKET=$(realpath "$2")
WORK=$PACKET/scratch/alsa
mkdir -p "$WORK"
cd "$WORK"
curl -fLsS https://www.alsa-project.org/files/pub/lib/alsa-lib-1.2.15.3.tar.bz2 -o alsa.tar.bz2
printf '%s  %s\n' 7b079d614d582cade7ab8db2364e65271d0877a37df8757ac4ac0c8970be861e alsa.tar.bz2 | sha256sum -c -
tar -xf alsa.tar.bz2
cd alsa-lib-1.2.15.3
./configure --host=aarch64-linux-gnu --prefix="$WORK/sysroot" --disable-python
make -j16
make install
cd "$REPO/milan-linux"
make -j16 MILAN_FPGA="$PACKET/scratch/fpga" O="$PACKET/scratch/cross" CC=aarch64-linux-gnu-gcc CFLAGS="-I$WORK/sysroot/include" LDFLAGS="-L$WORK/sysroot/lib" all
sha256sum "$PACKET"/scratch/cross/milan-*
