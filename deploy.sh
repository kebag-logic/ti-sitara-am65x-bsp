#!/bin/bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

UBOOT_PATH=${SCRIPT_DIR}/u-boot-official
TIBOOT3_BIN_PATH=${UBOOT_PATH}/out/r5/tiboot3.bin
TISPL_BIN_PATH=${UBOOT_PATH}/out/a53/tispl.bin
UBOOT_BIN_PATH=${UBOOT_PATH}/out/a53/u-boot.img

#dfu-util -S 01.00.00.00 -R -a bootloader -D $TIBOOT3_BIN_PATH
#sleep 0.5 
#snagrecover -s am625 -f ${SCRIPT_DIR}/snagboot/src/snagrecover/templates/am62x-beagle-play.yaml
