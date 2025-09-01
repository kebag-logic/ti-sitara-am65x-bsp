#!/bin/bash

set -x
# Most of the documentation is found here:
# https://software-dl.ti.com/processor-sdk-linux/esd/AM62X/latest/exports/docs/linux/Foundational_Components/U-Boot/BG-Build-K3.html
# https://docs.u-boot.org/en/stable/board/ti/k3.html

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

export CC32=arm-none-linux-gnueabihf-
export CC64=aarch64-linux-gnu-

export LNX_FW_PATH=${SCRIPT_DIR}/ti-linux-firmware/
export OPTEE_PATH=${SCRIPT_DIR}/optee_os
export UBOOT_DIR=${SCRIPT_DIR}/u-boot-official
export UBOOT_DIR_PB=${SCRIPT_DIR}/u-boot-pb
export TFA_PATH=${SCRIPT_DIR}/trusted-firmware-a/

export TFA_BOARD=lite
export OPTEE_PLATFORM=k3-am62x

# Bin output
export UB_R5_PATH=${UBOOT_DIR_PB}/out/r5
export UB_A53_PATH=${UBOOT_DIR_PB}/out/a53
export BL31_PATH=$TFA_PATH/build/k3/$TFA_BOARD/release/bl31.bin
export TEE_PATH=$OPTEE_PATH/out/arm-plat-k3/core/tee-raw.bin

export PB2_A53_CONFIG=am6232_pocketbeagle2_a53_defconfig
export PB2_R5_DFU_CONFIG="am6232_pocketbeagle2_r5_defconfig am62x_r5_usbdfu.config"
export PB2_R5_CONFIG=am6232_pocketbeagle2_r5_defconfig

cd $TFA_PATH
#Prepare the A53 to be wokenup
make CROSS_COMPILE=$CC64 ARCH=aarch64 PLAT=k3 SPD=opteed TARGET_BOARD=$TFA_BOARD

cd $UBOOT_DIR_PB
#Prepare the R5 Wakeup Domain processor
make ARCH=arm CROSS_COMPILE=$CC32 ${PB2_R5_CONFIG}  O=$UB_R5_PATH -j$(nproc)
make ARCH=arm CROSS_COMPILE=$CC32 O=$UB_R5_PATH  BINMAN_INDIRS=${LNX_FW_PATH} -j$(nproc)

cd $OPTEE_PATH
make CROSS_COMPILE=$CC32 CROSS_COMPILE64=$CC64 CFG_ARM64_core=y $OPTEE_EXTRA_ARGS \
	      PLATFORM=$OPTEE_PLATFORM -j$(nproc)
##
###build A53 uboot
cd $UBOOT_DIR_PB
make ARCH=arm CROSS_COMPILE=$CC64 $PB2_A53_CONFIG O=$UB_A53_PATH
make ARCH=arm CROSS_COMPILE=$CC64 BINMAN_INDIRS=$LNX_FW_PATH  O=$UB_A53_PATH \
	       BL31=$BL31_PATH TEE=$TEE_PATH -j$(nproc)

#mkimage -A arm64 -O linux -T kernel -C none -a 0x80008000 -e 0x80008000 -n "Linux kernel" -d linux/arch/arm/boot/Image uImage
mkimage -r -f fitImage.its fitimage #-k $UBOOT_PATH/arch/arm/mach-k3/keys -K $UBOOT_PATH/build/$ARMV8/dts/dt.dtb fitImage
