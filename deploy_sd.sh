#!/bin/bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

UBOOT_PATH=${SCRIPT_DIR}/u-boot-official
TIBOOT3_BIN_PATH=${UBOOT_PATH}/out_myir/r5/tiboot3-am62x-gp-myc-am62x.bin
TISPL_BIN_PATH=${UBOOT_PATH}/out_myir/a53/tispl.bin_unsigned
UBOOT_BIN_PATH=${UBOOT_PATH}/out_myir/a53/u-boot.img

#mount /dev/sda1 /run/media/alex/BOOT

#cp uImage  /run/media/alex/BOOT
cp $UBOOT_BIN_PATH /run/media/alex/BOOT/
cp $TIBOOT3_BIN_PATH  /run/media/alex/BOOT/tiboot3.bin
cp $TISPL_BIN_PATH  /run/media/alex/BOOT/tispl.bin
