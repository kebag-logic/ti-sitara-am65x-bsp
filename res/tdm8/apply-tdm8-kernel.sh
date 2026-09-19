#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: MIT

# Stage the out-of-tree kernel changes the TDM8 builds need into a kernel source
# tree: the KL TDM8 codec shim, plus every patch in res/tdm8/patches/. Idempotent:
# safe to re-run after `build-am62-kernel.sh fetch` re-clones ./linux, and after a
# kernel bump.
# Usage: apply-tdm8-kernel.sh [<linux-src>]   (default: ../../linux)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
KSRC=${1:-$(cd "$HERE/../.." && pwd)/linux}
CODECS="$KSRC/sound/soc/codecs"
PATCHES="$HERE/patches"

[ -f "$KSRC/Makefile" ] || { echo "not a kernel tree: $KSRC" >&2; exit 1; }

cp "$HERE/kl-tdm8-dummy.c" "$CODECS/kl-tdm8-dummy.c"

if ! grep -q 'SND_SOC_KL_TDM8_DUMMY' "$CODECS/Kconfig"; then
	# insert before the final endmenu so the symbol lands inside the codec menu
	last=$(grep -n '^endmenu$' "$CODECS/Kconfig" | tail -1 | cut -d: -f1)
	awk -v at="$last" '
	NR == at {
		print "config SND_SOC_KL_TDM8_DUMMY"
		print "\ttristate \"Kebag-Logic control-less 8-slot TDM peer\""
		print "\thelp"
		print "\t  Codec-side DAI shim for a TDM8 peer with no control bus, such"
		print "\t  as the Kebag-Logic AVB FPGA wired to McASP1 on the MYIR"
		print "\t  MYD-YM62X J11 header. The multichannel equivalent of"
		print "\t  linux,spdif-dit. Say M here to build snd-soc-kl-tdm8-dummy."
		print ""
	}
	{ print }
	' "$CODECS/Kconfig" > "$CODECS/Kconfig.tdm8" && mv "$CODECS/Kconfig.tdm8" "$CODECS/Kconfig"
	echo "Kconfig: added SND_SOC_KL_TDM8_DUMMY"
else
	echo "Kconfig: SND_SOC_KL_TDM8_DUMMY already present"
fi

if ! grep -q 'kl-tdm8-dummy' "$CODECS/Makefile"; then
	{
		echo ""
		echo "# Kebag-Logic control-less TDM8 peer shim"
		echo "snd-soc-kl-tdm8-dummy-y := kl-tdm8-dummy.o"
		echo "obj-\$(CONFIG_SND_SOC_KL_TDM8_DUMMY)	+= snd-soc-kl-tdm8-dummy.o"
	} >> "$CODECS/Makefile"
	echo "Makefile: added snd-soc-kl-tdm8-dummy.o"
else
	echo "Makefile: snd-soc-kl-tdm8-dummy.o already present"
fi

# SoC-level kernel fixes shared by all three AM62x boards, applied in lexical
# order. $KSRC is a git clone when a build script fetched it and a plain source
# tree otherwise, so drive `git apply` when there is a git dir and patch(1) when
# there is not.
if [ -e "$KSRC/.git" ]; then
	patch_applied() { git -C "$KSRC" apply --reverse --check "$1" >/dev/null 2>&1; }
	patch_fits()    { git -C "$KSRC" apply --check "$1" >/dev/null 2>&1; }
	patch_apply()   { git -C "$KSRC" apply "$1"; }
else
	command -v patch >/dev/null 2>&1 || \
		{ echo "no patch(1): cannot stage res/tdm8/patches into $KSRC" >&2; exit 1; }
	# -F0: patch(1) otherwise ignores up to two lines of mismatched context and
	# silently fuzzes a hunk into a tree it no longer fits, which is the one
	# outcome this script must never produce. Zero fuzz matches `git apply`.
	patch_applied() { patch -d "$KSRC" -p1 -F0 -R -f -s --dry-run < "$1" >/dev/null 2>&1; }
	patch_fits()    { patch -d "$KSRC" -p1 -F0 -f -s --dry-run < "$1" >/dev/null 2>&1; }
	patch_apply()   { patch -d "$KSRC" -p1 -F0 -f -s < "$1"; }
fi

# Idempotent, and all-or-nothing: an already-applied patch is recognised by a
# reverse dry-run and skipped, and a patch is dry-run before it is applied, so
# one that no longer fits stops the script instead of half-patching the tree.
for p in "$PATCHES"/*.patch; do
	[ -f "$p" ] || continue          # no patches: the glob stayed literal
	n=$(basename "$p")
	if patch_applied "$p"; then
		echo "patch: $n already applied"
	elif patch_fits "$p"; then
		patch_apply "$p"
		echo "patch: applied $n"
	else
		echo "patch: $n neither applies to $KSRC nor is already applied" >&2
		exit 1
	fi
done

echo "TDM8 shim + kernel patches applied to $KSRC"
