#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: MIT

# Test the PocketBeagle 2's A/B bootchooser (res/ab/pb2-boot.cmd.in, issue
# #27) in U-Boot's sandbox, built from the same fork the board boots
# (u-boot-pb), against an A/B card image (build-tdm8-uac2-pb2.sh abcard).
#
# The board has no serial console on the Ethernet Cap side, so a bootchooser
# that goes wrong costs a reflash. Here the same hush, setexpr and ext4 code
# runs on the host instead:
#
#   - the script, with "mmc 1:" read as the image bound as host disk 0, and
#     "reset" as "exit": each boot is the script sourced again, in the same
#     process, with the environment it left;
#   - booti cannot start an arm64 kernel in the sandbox, so every boot
#     "fails": exactly the path of a slot whose kernel does not come up;
#   - the sandbox takes the image formats the board's U-Boot takes
#     (LEGACY_IMAGE_FORMAT as in out_bp2/a53), and its boot.scr is wrapped by
#     the same res/ab/pb2-mkscr.sh as the card's: a script the board would
#     turn away is turned away here too.
#
# Checks:
#   B0  the card's own /boot.scr: a FIT whose hash checks out, holding
#       pb2-boot.cmd.in (the kernel arguments aside)
#   B1  from a fresh environment: A three times, then B three times, then
#       both get their attempts back and A goes again
#   B2  each boot reads /boot/Image.gz and the device tree from its own slot
#   B3  BOOT_ORDER="B A", as `rauc install` leaves it: B boots first
#
# Usage: test-pb2-bootchooser.sh <ab.img>     env: SANDBOX_O (sandbox build dir)
# Exit 0 = pass.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
BSP=$(cd "$HERE/../.." && pwd)
IMG=${1:?usage: $0 <ab.img>}
UB_SRC="$BSP/u-boot-pb"
O=${SANDBOX_O:-$BSP/u-boot-pb/out_sandbox}
MKIMAGE="$UB_SRC/out_bp2/a53/tools/mkimage"

BOARD_CONFIG="$UB_SRC/out_bp2/a53/.config"

[ -s "$IMG" ] || { echo "no image $IMG" >&2; exit 2; }
[ -s "$BOARD_CONFIG" ] || { echo "no board U-Boot build ($BOARD_CONFIG): ./build.sh PB2" >&2; exit 2; }

# ---- the sandbox ----

# does this U-Boot source legacy (mkimage -T script) images? <.config>
legacy_of() {
	if grep -q '^CONFIG_LEGACY_IMAGE_FORMAT=y' "$1" 2>/dev/null; then
		echo y
	else
		echo n
	fi
}

board_legacy=$(legacy_of "$BOARD_CONFIG")

if [ ! -x "$O/u-boot" ] || [ "$(legacy_of "$O/.config")" != "$board_legacy" ]; then
	echo "building the sandbox in $O (legacy images: $board_legacy, as the board)"

	make -s -C "$UB_SRC" O="$O" sandbox_defconfig

	# what the bootchooser does not need, and this fork's sandbox does not
	# link without
	"$UB_SRC/scripts/config" --file "$O/.config" \
		-d EFI_LOADER -d CMD_BOOTEFI -d BOOTMETH_EFILOADER \
		-d EFI_CAPSULE_AUTHENTICATE -d EFI_CAPSULE_ON_DISK -d EFI_SECURE_BOOT \
		-d UNIT_TEST -d UT_BOOTSTD -d UT_DM \
		-d CMD_UPL -d UPL

	# the image formats the board's U-Boot takes
	if [ "$board_legacy" = y ]; then
		"$UB_SRC/scripts/config" --file "$O/.config" -e LEGACY_IMAGE_FORMAT
	else
		"$UB_SRC/scripts/config" --file "$O/.config" -d LEGACY_IMAGE_FORMAT
	fi

	make -s -C "$UB_SRC" O="$O" olddefconfig
	make -s -C "$UB_SRC" O="$O" NO_SDL=1 -j"$(nproc)"
fi

[ "$(legacy_of "$O/.config")" = "$board_legacy" ] || { echo "the sandbox's image formats differ from the board's" >&2; exit 2; }

[ -x "$MKIMAGE" ] || MKIMAGE="$O/tools/mkimage"

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

sed -e 's|@APPEND@|console=ttyS3 rootwait|' \
    -e 's|mmc 1:|host 0:|g' \
    -e 's|^\treset$|\texit|' \
    -e 's|^reset$|exit|' \
    "$HERE/pb2-boot.cmd.in" > "$W/boot.cmd"

# wrapped as the card's is
"$HERE/pb2-mkscr.sh" "$MKIMAGE" "$W/boot.cmd" "$W/boot.scr"

# Run <n> boots after <setup>; print one line per boot: "<slot> <kernel bytes> <dtb bytes>"
boots() { # <n> <setup commands>
	local n=$1 setup=$2 cmds i

	cmds="host bind 0 $IMG; setenv kernel_addr_r 0x1000000; setenv fdt_addr_r 0x3000000; setenv loadaddr 0x4000000; $setup"

	for i in $(seq 1 "$n"); do
		cmds="$cmds; echo BOOT; load hostfs - \${loadaddr} $W/boot.scr; source \${loadaddr}"
	done

	"$O/u-boot" -D -c "$cmds" 2>&1 |
		sed 's/\x1b\[[0-9;?]*[A-Za-z]//g; s/\r//g' |
		awk '
			/^BOOT$/ { if (n) print slot, k, d; n++; slot = "-"; k = 0; d = 0; reads = 0 }
			/^BOOTCHOOSER: slot [AB] \(/ { slot = $3 }
			/^BOOTCHOOSER: both slots exhausted/ { slot = "refill" }
			/bytes read/ { reads++; if (reads == 2) k = $1; if (reads == 3) d = $1 }
			END { if (n) print slot, k, d }'
}

fail=0

check() { # <id> <what> <ok>
	if [ "$3" = 1 ]; then
		echo "PASS $1 $2"
	else
		echo "FAIL $1 $2"
		fail=1
	fi
}

# ---- B0 ----

# the card's /boot.scr, off p1. Not sourced: its `reset` would relaunch the
# sandbox. `iminfo` checks what `source` would check first: the format and
# the hash.
p1=$(sfdisk -d "$IMG" | awk -F'[=,]' '/start=/ { gsub(/ /, "", $2); print $2; exit }')
mcopy -n -i "$IMG@@$((p1 * 512))" ::/boot.scr "$W/card.scr"

# (what iminfo says, after the load: not the sandbox's own start-up)
info=$("$O/u-boot" -D -c "load hostfs - \${loadaddr} $W/card.scr; iminfo \${loadaddr}" 2>&1 |
	sed 's/\x1b\[[0-9;?]*[A-Za-z]//g; s/\r//g' |
	sed -n '/bytes read/,$p')

format_ok=0
if echo "$info" | grep -q 'FIT image found' &&
   echo "$info" | grep -q "Default Configuration: 'conf-1'" &&
   echo "$info" | grep -q 'sha256+' &&
   ! echo "$info" | grep -qi 'error\|unknown image format'; then
	format_ok=1
fi

# what it runs: pb2-boot.cmd.in, with the label's arguments in place of @APPEND@
: > "$W/card.cmd"
"$(dirname "$MKIMAGE")/dumpimage" -T flat_dt -p 0 -o "$W/card.cmd" "$W/card.scr" >/dev/null 2>&1 || true

text_ok=0
if diff -q <(grep -v '^setenv cargs ' "$W/card.cmd") \
           <(grep -v '^setenv cargs ' "$HERE/pb2-boot.cmd.in") >/dev/null; then
	text_ok=1
fi

check B0 "the card's boot.scr is a FIT with a good hash ($format_ok) and holds pb2-boot.cmd.in ($text_ok)" \
	"$([ "$format_ok$text_ok" = 11 ] && echo 1 || echo 0)"

# ---- B1, B2 ----

out=$(boots 8 "")
slots=$(echo "$out" | awk '{ printf "%s ", $1 }')
echo "fresh environment, 8 failing boots: $slots"

check B1 "A A A B B B, refill, A" "$([ "$slots" = "A A A B B B refill A " ] && echo 1 || echo 0)"

kernel=$(echo "$out" | awk '$1 == "A" || $1 == "B" { print $2 }' | sort -u)
dtb=$(echo "$out" | awk '$1 == "A" || $1 == "B" { print $3 }' | sort -u)
check B2 "every boot read the kernel ($kernel bytes) and the device tree ($dtb bytes)" \
	"$([ -n "$kernel" ] && [ "$kernel" != 0 ] && [ "$(echo "$kernel" | wc -l)" = 1 ] && [ -n "$dtb" ] && [ "$dtb" != 0 ] && echo 1 || echo 0)"

# ---- B3 ----

out=$(boots 1 'setenv BOOT_ORDER "B A"; setenv BOOT_A_LEFT 3; setenv BOOT_B_LEFT 3')
slots=$(echo "$out" | awk '{ printf "%s ", $1 }')
echo "BOOT_ORDER=\"B A\": $slots"

check B3 "after rauc install, B boots first" "$([ "$slots" = "B " ] && echo 1 || echo 0)"

exit "$fail"
