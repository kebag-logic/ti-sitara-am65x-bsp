#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: MIT

# Add the KL TDM8 codec shim to a kernel source tree. Idempotent: safe to re-run
# after `build-am62-kernel.sh fetch` re-clones ./linux, and after a kernel bump.
# Usage: apply-tdm8-kernel.sh [<linux-src>]   (default: ../../linux)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
KSRC=${1:-$(cd "$HERE/../.." && pwd)/linux}
CODECS="$KSRC/sound/soc/codecs"

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

echo "TDM8 shim applied to $KSRC"
