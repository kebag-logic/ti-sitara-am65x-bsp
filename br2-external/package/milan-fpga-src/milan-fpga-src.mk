################################################################################
#
# milan-fpga-src
#
################################################################################

# One pin for the host build and this one: milan-linux/milan-fpga.pin
MILAN_FPGA_SRC_PIN = $(BR2_EXTERNAL_KL_AM62X_PATH)/../milan-linux/milan-fpga.pin
MILAN_FPGA_SRC_VERSION = $(shell sed -n 's/^MILAN_FPGA_REV=//p' $(MILAN_FPGA_SRC_PIN))
MILAN_FPGA_SRC_SITE = https://github.com/kebag-logic/milan-fpga/archive
MILAN_FPGA_SRC_SOURCE = $(MILAN_FPGA_SRC_VERSION).tar.gz
MILAN_FPGA_SRC_LICENSE = CERN-OHL-W-2.0
MILAN_FPGA_SRC_LICENSE_FILES = LICENSE
MILAN_FPGA_SRC_INSTALL_TARGET = NO

$(eval $(generic-package))
