#!/bin/sh
set -eu
repo=$(realpath "$1")
packet=$(realpath "$2")
export TMPDIR="$packet/scratch"
export PYTHONDONTWRITEBYTECODE=1
mkdir -p "$TMPDIR"
cd "$repo/milan-linux"
set +e
make -j16 MILAN_FPGA="$packet/scratch/upstream" O="$packet/scratch/native" check > "$packet/make-check.log" 2>&1
rc=$?
printf '%s\n' "$rc" > "$packet/make-check.rc"
tail -12 "$packet/make-check.log"
exit "$rc"
