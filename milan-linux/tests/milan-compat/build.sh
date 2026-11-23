#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0

# Build milan-compat against a la_avdecc 4.3.x checkout and its CMake build
# tree (the static libraries la_avdecc_controller_static, la_avdecc_static and
# la_networkInterfaceHelper_static, with the pcap protocol interface):
#
#   git clone --recursive --branch v4.3.1.1 https://github.com/L-Acoustics/avdecc SRC
#   cmake -S SRC -B BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release
#   ninja -C BUILD la_avdecc_controller_static
#   tests/milan-compat/build.sh SRC BUILD OUT
#
# The library's public compile options (its exported targets') are repeated
# here, since they change its classes' layouts.
#
# usage: build.sh <la_avdecc source> <la_avdecc build> <output binary>

set -e
SRC=${1:?usage: build.sh <la_avdecc source> <la_avdecc build> <output binary>}
BUILD=${2:?}
OUT=${3:?}
HERE=$(cd "$(dirname "$0")" && pwd)
${CXX:-c++} -std=gnu++17 -O2 -Wall -Wextra -Werror \
	-DTARGET_SYSTEM_LINUX -DENABLE_AVDECC_FEATURE_REDUNDANCY -DENABLE_AVDECC_STRICT_2018_REDUNDANCY \
	-DENABLE_AVDECC_FEATURE_JSON -DENABLE_AVDECC_FEATURE_CBR -DALLOW_RECV_BIG_AECP_PAYLOADS \
	-Dla_avdecc_static_STATICS -Dla_avdecc_controller_static_STATICS -Dla_networkInterfaceHelper_static_STATICS \
	-I"$SRC/include" -I"$SRC/externals/nih/include" -I"$SRC/externals/3rdparty/json/include" \
	-o "$OUT" "$HERE/milan_compat.cpp" \
	"$BUILD/src/controller/libla_avdecc_controller_static.a" "$BUILD/src/libla_avdecc_static.a" \
	"$BUILD/externals/nih/src/libla_networkInterfaceHelper_static.a" -lpthread -ldl
