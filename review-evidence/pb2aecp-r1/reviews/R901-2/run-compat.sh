#!/bin/sh
set -eu
REPO=$(realpath "$1")
PACKET=$(realpath "$2")
export TMPDIR=$PACKET/scratch
SRC=$PACKET/scratch/avdecc
BUILD=$PACKET/scratch/avdecc-strict
git -C "$SRC" rev-parse HEAD
cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_AVDECC_LIB_SHARED_CXX=OFF \
  -DIGNORE_INVALID_CONTROL_DATA_LENGTH=OFF -DIGNORE_INVALID_NON_SUCCESS_AEM_RESPONSES=OFF \
  -DIGNORE_NEITHER_STATIC_NOR_DYNAMIC_MAPPINGS=OFF -DCONTINUE_MISBEHAVE_AEM_RESPONSES=OFF \
  -DALLOW_GET_AUDIO_MAP_UNSOL=OFF
cmake --build "$BUILD" --target la_avdecc_controller_static --parallel 4
make -C "$REPO/milan-linux" -j16 MILAN_FPGA="$PACKET/scratch/fpga" O="$PACKET/scratch/native" LA_AVDECC_SRC="$SRC" LA_AVDECC_BUILD="$BUILD" check-compat
