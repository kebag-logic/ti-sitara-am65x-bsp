#!/bin/sh
# Reproduce the native exact-head gate. REPO and PACKET are positional arguments.
set -eu
REPO=$(realpath "$1")
PACKET=$(realpath "$2")
mkdir -p "$PACKET/scratch"
export TMPDIR=$PACKET/scratch
"$REPO/milan-linux/tools/fetch-milan-fpga.sh" "$PACKET/scratch/fpga" --with-submodules
make -C "$REPO/milan-linux" -j16 MILAN_FPGA="$PACKET/scratch/fpga" O="$PACKET/scratch/native" check
