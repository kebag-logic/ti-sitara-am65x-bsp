#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: MIT

# Verify a built K3 bootloader chain is the one you meant to flash, before it
# reaches a card.  Written for the PocketBeagle 2 HS-FS chain, but the SOC and
# VARIANT knobs cover the SK and MYIR boards too.
#
# Why this exists
# ---------------
# Two failure modes here are both SILENT:
#
#  1. Wrong security variant.  The ROM rejects a tiboot3 signed/packaged for
#     another device type before the UART comes up, so the board just looks
#     dead.  binman builds every variant it knows into the same directory, so
#     picking the wrong filename is one keystroke away.
#
#  2. Fake blobs.  U-Boot's binman is invoked with `--allow-missing
#     --fake-ext-blobs` (see cmd_binman in the U-Boot Makefile), so a build with
#     no BINMAN_INDIRS still SUCCEEDS - it just substitutes zero-byte stand-ins
#     for the TI firmware and writes them to <out>/binman-fake/.  The resulting
#     tiboot3.bin is the right size, has a real certificate, and contains no
#     TIFS at all.  Worse, the TIFS stub inside tispl.bin is declared
#     `optional;` in k3-*-binman.dtsi, so binman does not even warn about it.
#
# So this checks the artifacts, not the build log: it looks for the actual TI
# firmware bytes inside the images.
#
# Usage: check-bootloader.sh [<uboot-out-dir>]      (default: u-boot-pb/out_bp2)
# Env:   SOC=am62x  VARIANT=hs-fs|hs|gp  FW=<ti-linux-firmware dir>
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BSP=$(cd "$HERE/../.." && pwd)
OUT=${1:-$BSP/u-boot-pb/out_bp2}
SOC=${SOC:-am62x}
VARIANT=${VARIANT:-hs-fs}
FW=${FW:-$BSP/ti-linux-firmware}

R5="$OUT/r5"
A53="$OUT/a53"
fails=0
warns=0
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; fails=$((fails + 1)); }
warn() { printf 'WARN  %s\n' "$*"; warns=$((warns + 1)); }
note() { printf '      %s\n' "$*"; }

# offset of <needle-file>'s first N bytes inside <haystack-file>, or -1
blob_at() {
	python3 - "$1" "$2" "${3:-256}" <<-'PY'
	import sys
	try:
	    hay = open(sys.argv[1], 'rb').read()
	    nee = open(sys.argv[2], 'rb').read()[:int(sys.argv[3])]
	except OSError:
	    print(-1); raise SystemExit
	print(hay.find(nee) if nee else -1)
	PY
}

# TIFS blob that the ROM-loaded image must carry, per variant.  These names come
# straight from the `filename = "ti-sysfw/..."` lines in k3-*-binman.dtsi.
case "$VARIANT" in
hs-fs) TIFS="$FW/ti-sysfw/ti-fs-firmware-$SOC-hs-fs-enc.bin" ;;
hs)    TIFS="$FW/ti-sysfw/ti-fs-firmware-$SOC-hs-enc.bin" ;;
gp)    TIFS="$FW/ti-sysfw/ti-fs-firmware-$SOC-gp.bin" ;;
*) echo "unknown VARIANT=$VARIANT (use hs-fs, hs or gp)" >&2; exit 2 ;;
esac
STUB="$FW/ti-sysfw/ti-fs-stub-firmware-$SOC-hs-enc.bin"

# Board vendors rename the image: the SK gets tiboot3-am62x-hs-fs-evm.bin, the
# MYIR one tiboot3-am62x-gp-myc-am62x.bin.  Classify by the token right after
# the SoC name instead of guessing the suffix - and note that a plain "hs-"
# prefix match would also swallow "hs-fs-", so test the longer one first.
variant_of() {
	case "${1#tiboot3-$SOC-}" in
	hs-fs-*) echo hs-fs ;;
	hs-*)    echo hs ;;
	gp-*)    echo gp ;;
	*)       echo "?" ;;
	esac
}
TIBOOT3=""
for f in "$R5"/tiboot3-"$SOC"-*.bin; do
	[ -f "$f" ] || continue
	[ "$(variant_of "$(basename "$f")")" = "$VARIANT" ] || continue
	TIBOOT3="$f"; break
done
[ -n "$TIBOOT3" ] || TIBOOT3="$R5/tiboot3-$SOC-$VARIANT-evm.bin"

echo "checking $OUT  (SOC=$SOC VARIANT=$VARIANT)"
echo

[ -d "$R5" ] || { echo "no $R5 - build the bootloaders first (./build.sh PB2)" >&2; exit 1; }
[ -s "$TIFS" ] || { echo "no TI firmware at $TIFS - set FW=<ti-linux-firmware>" >&2; exit 1; }

# --- 1. did binman fake anything? --------------------------------------------
# binman always CREATES <out>/binman-fake/; it only puts files there for blobs
# it could not find, and it does not clean them up on a later good build.  So a
# populated directory means "at least one build here ran without the firmware",
# not necessarily "these images are bad" - the embedded-blob checks below are
# the authoritative ones.
faked=0
for d in "$R5" "$A53"; do
	[ -d "$d/binman-fake" ] || continue
	n=$(ls -A "$d/binman-fake" 2>/dev/null | wc -l)
	[ "$n" -eq 0 ] && continue
	faked=$((faked + n))
	warn "$d/binman-fake holds $n stand-in(s) from some build in this directory:"
	for f in "$d"/binman-fake/*; do
		[ -e "$f" ] && note "  $(basename "$f") ($(stat -c%s "$f") bytes)"
	done
done
if [ "$faked" -eq 0 ]; then
	pass "no binman stand-ins left in either output dir"
else
	note "  stale fakes are harmless; the embedded-blob checks below decide"
	note "  rebuild with BINMAN_INDIRS=$FW if those fail"
fi

# --- 2. the right tiboot3 exists, and tiboot3.bin points at it ----------------
if [ -s "$TIBOOT3" ]; then
	pass "$(basename "$TIBOOT3") present ($(stat -c%s "$TIBOOT3") bytes)"
else
	fail "$(basename "$TIBOOT3") missing from $R5"
	note "  built variants: $(cd "$R5" 2>/dev/null && ls tiboot3-*.bin 2>/dev/null | tr '\n' ' ')"
fi
if [ -L "$R5/tiboot3.bin" ]; then
	link=$(basename "$(readlink "$R5/tiboot3.bin")")
	if [ "$link" = "$(basename "$TIBOOT3")" ]; then
		pass "tiboot3.bin -> $link"
	else
		warn "tiboot3.bin -> $link, not the $VARIANT image"
		note "  name the file explicitly rather than following the symlink"
	fi
else
	warn "no tiboot3.bin symlink in $R5 - name the $VARIANT file explicitly"
fi

# --- 3. the real TIFS is inside it, and it is the right one ------------------
if [ -s "$TIBOOT3" ]; then
	at=$(blob_at "$TIBOOT3" "$TIFS")
	if [ "$at" -ge 0 ]; then
		pass "$(basename "$TIFS") embedded at offset $at"
	else
		fail "$(basename "$TIFS") is NOT inside $(basename "$TIBOOT3")"
		note "  the image carries no usable TIFS; the ROM will stop before the UART"
		note "  rebuild with BINMAN_INDIRS=$FW"
	fi
	# a GP TIFS in a slot that should hold an HS one means the wrong variant
	if [ "$VARIANT" != gp ] && [ -s "$FW/ti-sysfw/ti-fs-firmware-$SOC-gp.bin" ]; then
		gpat=$(blob_at "$TIBOOT3" "$FW/ti-sysfw/ti-fs-firmware-$SOC-gp.bin")
		[ "$gpat" -ge 0 ] && fail "this image carries the GP TIFS - it is a GP image, not $VARIANT"
	fi
fi

# --- 4. the ROM certificate ---------------------------------------------------
cert="$R5/cert.$(basename "$TIBOOT3").ti-secure-rom"
if [ -s "$cert" ]; then
	subj=$(openssl x509 -in "$cert" -noout -subject 2>/dev/null | sed 's/^subject=//')
	if [ -n "$subj" ]; then
		pass "ROM certificate parses"
		note "  signed by: $subj"
	else
		fail "$(basename "$cert") is not a readable x509 certificate"
	fi
else
	warn "no $(basename "$cert") - cannot confirm the image was signed"
fi

# --- 5. the A53 half: signed tispl.bin / u-boot.img ---------------------------
# On HS-FS and HS you flash tispl.bin and u-boot.img. The *_unsigned twins are
# the GP ones; they exist in the same directory and are easy to grab by mistake.
if [ -d "$A53" ]; then
	for pair in "tispl.bin tispl.bin_unsigned" "u-boot.img u-boot.img_unsigned"; do
		set -- $pair
		s="$A53/$1"; u="$A53/$2"
		if [ ! -s "$s" ]; then
			fail "$1 missing from $A53"
		elif [ "$VARIANT" = gp ]; then
			[ -s "$u" ] && pass "$2 present ($(stat -c%s "$u") bytes) - the GP one to flash"
		elif [ -s "$u" ] && [ "$(stat -c%s "$s")" -eq "$(stat -c%s "$u")" ]; then
			fail "$1 is byte-identical in size to $2 - signing did not run"
		else
			pass "$1 present and larger than $2 (signed)"
		fi
	done

	# The TIFS stub rides inside tispl.bin and is declared `optional;` in
	# binman, so a build without BINMAN_INDIRS drops it without a word.
	if [ -s "$A53/tispl.bin" ] && [ -s "$STUB" ]; then
		at=$(blob_at "$A53/tispl.bin" "$STUB")
		if [ "$at" -ge 0 ]; then
			pass "TIFS stub embedded in tispl.bin at offset $at"
		else
			fail "tispl.bin carries no TIFS stub ($(basename "$STUB"))"
			note "  binman marks it 'optional', so this fails silently - rebuild with BINMAN_INDIRS=$FW"
		fi
	fi
else
	warn "no $A53 - only the R5 half was checked"
fi

# --- verdict ------------------------------------------------------------------
echo
if [ "$fails" -ne 0 ]; then
	echo "$fails check(s) FAILED, $warns warning(s) - do not flash this chain"
	exit 1
fi
[ "$warns" -ne 0 ] && echo "$warns warning(s), no failures"

# HS and HS-FS take the signed pair.  U-Boot's doc/board/ti/k3.rst names the
# *_unsigned pair for GP - though this BSP's GP board boots off the signed one
# too, since GP silicon does not authenticate either way.
if [ "$VARIANT" = gp ]; then
	SPL_OUT="$A53/tispl.bin_unsigned"; UB_OUT="$A53/u-boot.img_unsigned"
else
	SPL_OUT="$A53/tispl.bin"; UB_OUT="$A53/u-boot.img"
fi
cat <<SUMMARY

$VARIANT chain verified. Flash these three, under these names:

  tiboot3.bin  <- $TIBOOT3
  tispl.bin    <- $SPL_OUT
  u-boot.img   <- $UB_OUT

build-spare-sd.sh takes them as:

  UBOUT=$OUT \\
  R5=$TIBOOT3 \\
  TISPL=$SPL_OUT \\
  UB=$UB_OUT

Confirm the silicon agrees before you trust this: the U-Boot banner prints
"SoC:   AM62X SR1.0 $(echo "$VARIANT" | tr 'a-z' 'A-Z')" on a matching board, and res/parse_uart_boot_socid.py
decodes the DeviceType out of a UART-boot SoC ID dump.
SUMMARY
