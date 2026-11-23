#!/bin/sh
set -eu
repo=$(realpath "$1")
packet=$(realpath "$2")
export TMPDIR="$packet/scratch"
mkdir -p "$TMPDIR"
"$repo/milan-linux/tools/fetch-milan-fpga.sh" "$packet/scratch/upstream" --with-submodules
"$repo/milan-linux/tools/gen-config.sh" --check "$packet/scratch/upstream"
