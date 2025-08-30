#!/bin/bash

#set -x
# Most of the documentation is found here:
# https://software-dl.ti.com/processor-sdk-linux/esd/AM62X/latest/exports/docs/linux/Foundational_Components/U-Boot/BG-Build-K3.html
# https://docs.u-boot.org/en/stable/board/ti/k3.html

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

LNX_FW_REPO=git://git.ti.com/processor-firmware/ti-linux-firmware.git
OPTEE_REPO=https://github.com/OP-TEE/optee_os/
UBOOT_PB_REPO=https://openbeagle.org/beagleboard/u-boot.git
TFA_REPO=https://review.trustedfirmware.org/TF-A/trusted-firmware-a.git

LNX_FW_PATH=${SCRIPT_DIR}/ti-linux-firmware/
OPTEE_PATH=${SCRIPT_DIR}/optee_os
UBOOT_PB_PATH=${SCRIPT_DIR}/u-boot-pb
TFA_PATH=${SCRIPT_DIR}/trusted-firmware-a/

git clone $LNX_FW_REPO $LNX_FW_PATH
git clone $OPTEE_REPO $OPTEE_PATH
git clone $UBOOT_PB_REPO -b v2025.04-rc4-pocketbeagle2 $UBOOT_PB_PATH
git clone $TFA_REPO $TFA_PATH
