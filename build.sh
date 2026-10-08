#!/bin/bash

set -x
# Most of the documentation is found here:
# https://software-dl.ti.com/processor-sdk-linux/esd/AM62X/latest/exports/docs/linux/Foundational_Components/U-Boot/BG-Build-K3.html
# https://docs.u-boot.org/en/stable/board/ti/k3.html

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

CC32=arm-none-linux-gnueabihf-
CC64=aarch64-linux-gnu-

LNX_FW_PATH=${SCRIPT_DIR}/ti-linux-firmware/
OPTEE_PATH=${SCRIPT_DIR}/optee_os
UBOOT_DIR=${SCRIPT_DIR}/u-boot-official
UBOOT_DIR_PB=${SCRIPT_DIR}/u-boot-pb
TFA_PATH=${SCRIPT_DIR}/trusted-firmware-a/

# This is for hte TARGET BOARD https://docs.u-boot.org/en/stable/board/ti/k3.html#building-tispl-bin
TFA_BOARD=lite
OPTEE_PLATFORM=k3-am62x


PB2_A53_CONFIG=am6232_pocketbeagle2_a53_defconfig
PB2_R5_DFU_CONFIG="am6232_pocketbeagle2_r5_defconfig am62x_r5_usbdfu.config"
PB2_R5_CONFIG=am6232_pocketbeagle2_r5_defconfig

# TI AM625 Starter Kit (SK-AM62B / SK-AM62B-P1). U-Boot calls the SK "evm";
# its control DTB is ti/k3-am625-sk, and binman symlinks tiboot3.bin to the
# HS-FS image, which is the silicon the SK-AM62B-P1 ships with.
SK_AM62B_A53_CONFIG=am62x_evm_a53_defconfig
SK_AM62B_R5_DFU_CONFIG="am62x_evm_r5_defconfig am62x_r5_usbdfu.config"
SK_AM62B_R5_CONFIG=am62x_evm_r5_defconfig

MYIR_AM6254_A53_CONFIG=myc_am62x_a53_defconfig
MYIR_AM6254_R5_DFU_CONFIG="myc_am62x_r5_defconfig am62x_r5_usbdfu.config"
MYIR_AM6254_R5_CONFIG=myc_am62x_r5_defconfig

# Below the default values
DEFAULT_OUT_FOLDER="myir"

# Which U-Boot tree this board builds from. PocketBeagle 2 is the odd one out:
# its support lives in BeagleBoard's fork (u-boot-pb, cloned by fetch.sh), so
# am6232_pocketbeagle2_*_defconfig does not exist in u-boot-official at all.
# Everything below the case statement uses UBOOT_SRC, not UBOOT_DIR.
UBOOT_SRC=$UBOOT_DIR

DEFAULT_A53_CONFIG=$MYIR_AM6254_A53_CONFIG
DEFAULT_R5_DFU_CONFIG=$MYIR_AM6254_R5_DFU_CONFIG
DEFAULT_R5_CONFIG=$MYIR_AM6254_R5_CONFIG
# Default patgh
UB_R5_PATH=${UBOOT_DIR}/out_${DEFAULT_OUT_FOLDER}/r5
UB_A53_PATH=${UBOOT_DIR}/out_${DEFAULT_OUT_FOLDER}/a53

BL31_PATH=$TFA_PATH/build/k3/$TFA_BOARD/release/bl31.bin
TEE_PATH=$OPTEE_PATH/out/arm-plat-k3/core/tee-raw.bin

echo "Starting building/image creator"
case $1 in
	'PB2')
		DEFAULT_OUT_FOLDER="bp2"
		UBOOT_SRC=$UBOOT_DIR_PB
		UB_R5_PATH=${UBOOT_DIR_PB}/out_${DEFAULT_OUT_FOLDER}/r5
		UB_A53_PATH=${UBOOT_DIR_PB}/out_${DEFAULT_OUT_FOLDER}/a53
		# HS-FS board: binman builds tiboot3-am62x-hs-fs-evm.bin (symlinked
		# to tiboot3.bin) plus the HS-SE one, and no GP image. Verify what
		# comes out with res/uboot/check-bootloader.sh - see README.silcons.
		[ -d "$UBOOT_DIR_PB" ] || {
			echo "PB2 needs BeagleBoard's U-Boot fork at $UBOOT_DIR_PB" >&2
			echo "run ./fetch.sh first (clones it at v2025.04-rc4-pocketbeagle2)" >&2
			exit 1
		}

		DEFAULT_A53_CONFIG=$PB2_A53_CONFIG
		DEFAULT_R5_DFU_CONFIG=$PB2_R5_DFU_CONFIG
		DEFAULT_R5_CONFIG=$PB2_R5_CONFIG
		;;
	'SK'|'SK-AM62B'|'SK-AM62B-P1')
		DEFAULT_OUT_FOLDER="sk"
		UB_R5_PATH=${UBOOT_DIR}/out_${DEFAULT_OUT_FOLDER}/r5
		UB_A53_PATH=${UBOOT_DIR}/out_${DEFAULT_OUT_FOLDER}/a53

		DEFAULT_A53_CONFIG=$SK_AM62B_A53_CONFIG
		DEFAULT_R5_DFU_CONFIG=$SK_AM62B_R5_DFU_CONFIG
		DEFAULT_R5_CONFIG=$SK_AM62B_R5_CONFIG
		;;
	'MYIR')
		;;
	*)
	echo "No board given (PB2, SK or MYIR), choosing default MYIR"
	;;
esac

echo "Going to build the ${DEFAULT_OUT_FOLDER}"
# Bin output

cd $TFA_PATH
#Prepare the A53 to be woken-up
make CROSS_COMPILE=$CC64 ARCH=aarch64 PLAT=k3 SPD=opteed TARGET_BOARD=$TFA_BOARD EARLY_CONSOLE=1 LOG_LEVEL=50

# U-Boot's vendored pylibfdt does not build with SWIG >= 4.3 (Arch ships 4.5.x),
# and binman needs it to assemble tiboot3.bin / tispl.bin. Idempotent no-op once
# applied, or on an older swig. See res/uboot/fix-pylibfdt-swig.sh.
${SCRIPT_DIR}/res/uboot/fix-pylibfdt-swig.sh "$UBOOT_SRC"

# binman <= v2025.04 imports pkg_resources, which setuptools 81 removed. Only
# u-boot-pb (PocketBeagle 2) is that old; a no-op on u-boot-official.
# See res/uboot/fix-binman-pkg-resources.sh.
${SCRIPT_DIR}/res/uboot/fix-binman-pkg-resources.sh "$UBOOT_SRC"

# PocketBeagle 2 only. All three are no-ops on any other tree, but run them
# just for PB2 so a MYIR/SK build cannot be surprised by them.
if [ "$1" = "PB2" ]; then
	# The A53 SPL's console pinmux group has no bootph-*, so fdtgrep drops it
	# and the SPL prints into an unmuxed UART: boot goes silent right after
	# "Entry point address = 0x80080000".
	${SCRIPT_DIR}/res/uboot/fix-pb2-uart6-bootph.sh "$UBOOT_SRC"

	# Without kernel_comp_addr_r / kernel_comp_size, booti refuses a gzipped
	# Image and every extlinux label fails with err=-14.
	${SCRIPT_DIR}/res/uboot/fix-pb2-kernel-comp.sh "$UBOOT_SRC"

	# The board splits its boot log across two UARTs - R5/TF-A/OP-TEE on
	# main_uart0 (P1.30/P1.32), A53 U-Boot and Linux on main_uart6 (JST-SH).
	# Default to putting the A53 stages on main_uart0 too, so the whole log
	# lands on one wire; PB2_CONSOLE_ON_P1=off keeps the stock split.
	${SCRIPT_DIR}/res/uboot/pb2-console-on-p1.sh "${PB2_CONSOLE_ON_P1:-on}" "$UBOOT_SRC"

	# A persistent, redundant environment on the microSD, which RAUC's A/B
	# bootchooser keeps its state in (issue #27); PB2_AB_ENV=off keeps the
	# stock CONFIG_ENV_IS_NOWHERE.
	${SCRIPT_DIR}/res/uboot/pb2-ab-env.sh "${PB2_AB_ENV:-on}" "$UBOOT_SRC"
fi

cd $UBOOT_SRC
#Prepare the R5 Wakeup Domain processor
make ARCH=arm CROSS_COMPILE=$CC32 ${DEFAULT_R5_CONFIG} O=${UB_R5_PATH} -j$(nproc) V=1
make ARCH=arm CROSS_COMPILE=$CC32 O=${UB_R5_PATH} BINMAN_INDIRS=${LNX_FW_PATH} -j$(nproc) -j$(nproc)


cd $OPTEE_PATH
make CROSS_COMPILE=$CC32 CROSS_COMPILE64=$CC64 CFG_ARM64_core=y $OPTEE_EXTRA_ARGS \
	      PLATFORM=$OPTEE_PLATFORM -j$(nproc)
#
##build A53 uboot
cd $UBOOT_SRC
make ARCH=arm CROSS_COMPILE=$CC64 $DEFAULT_A53_CONFIG O=$UB_A53_PATH
make ARCH=arm CROSS_COMPILE=$CC64 BINMAN_INDIRS=$LNX_FW_PATH  O=$UB_A53_PATH \
	       BL31=$BL31_PATH TEE=$TEE_PATH -j$(nproc)

##mkimage -A arm64 -O linux -T kernel -C none -a 0x80008000 -e 0x80008000 -n "Linux kernel" -d linux/arch/arm/boot/Image uImage
#mkimage -r -f fitImage.its fitimage #-k $UBOOT_PATH/arch/arm/mach-k3/keys -K $UBOOT_PATH/build/$ARMV8/dts/dt.dtb fitImage

# This script has no `set -e`, and a binman failure does not stop the make it
# runs under - so a run that produced no bootloader at all still reaches here.
# That is exactly how an "OK" build ends up with an empty output directory.
# Check the artifacts before claiming success.
missing=""
ls ${UB_R5_PATH}/tiboot3*.bin >/dev/null 2>&1 || missing="$missing tiboot3*.bin"
[ -s "${UB_A53_PATH}/tispl.bin" ]   || missing="$missing tispl.bin"
[ -s "${UB_A53_PATH}/u-boot.img" ]  || missing="$missing u-boot.img"
if [ -n "$missing" ]; then
	set +x
	echo "" >&2
	echo "build.sh: BUILD INCOMPLETE for ${DEFAULT_OUT_FOLDER} - missing:$missing" >&2
	echo "  Search the output above for 'Error' - binman and make sub-builds" >&2
	echo "  fail without aborting this script." >&2
	exit 1
fi
set +x
echo ""
echo "build.sh: ${DEFAULT_OUT_FOLDER} bootloaders built"
echo "  R5  : ${UB_R5_PATH}"
echo "  A53 : ${UB_A53_PATH}"
echo "  verify: ./res/uboot/check-bootloader.sh $(dirname ${UB_R5_PATH})"
