################################################################################
#
# milan-fpga-src
#
################################################################################

# One pin for the host build and this one: milan-linux/milan-fpga.pin. The
# firmware's ADP, ACMP, MAAP and wire layer are the tsn-c-stack submodule's,
# and AECP's bridge includes lwSRP's headers, so both archives are unpacked
# where their gitlinks sit (third_party/), as tools/fetch-milan-fpga.sh does.
MILAN_FPGA_SRC_PIN = $(BR2_EXTERNAL_KL_AM62X_PATH)/../milan-linux/milan-fpga.pin
MILAN_FPGA_SRC_VERSION = $(shell sed -n 's/^MILAN_FPGA_REV=//p' $(MILAN_FPGA_SRC_PIN))
MILAN_FPGA_SRC_SITE = https://github.com/kebag-logic/milan-fpga/archive
MILAN_FPGA_SRC_SOURCE = $(MILAN_FPGA_SRC_VERSION).tar.gz
MILAN_FPGA_SRC_LICENSE = CERN-OHL-W-2.0
MILAN_FPGA_SRC_LICENSE_FILES = LICENSE
MILAN_FPGA_SRC_INSTALL_TARGET = NO

MILAN_FPGA_SRC_TSN_REV = $(shell sed -n 's/^TSN_C_STACK_REV=//p' $(MILAN_FPGA_SRC_PIN))
MILAN_FPGA_SRC_LWSRP_REV = $(shell sed -n 's/^LWSRP_REV=//p' $(MILAN_FPGA_SRC_PIN))
MILAN_FPGA_SRC_EXTRA_DOWNLOADS = \
	https://github.com/kebag-logic/tsn-c-stack/archive/$(MILAN_FPGA_SRC_TSN_REV).tar.gz \
	https://github.com/kebag-logic/lwSRP/archive/$(MILAN_FPGA_SRC_LWSRP_REV).tar.gz

define MILAN_FPGA_SRC_EXTRACT_SUBMODULES
	mkdir -p $(@D)/third_party/tsn-c-stack $(@D)/third_party/lwSRP
	$(call suitable-extractor,$(MILAN_FPGA_SRC_TSN_REV).tar.gz) \
		$(MILAN_FPGA_SRC_DL_DIR)/$(MILAN_FPGA_SRC_TSN_REV).tar.gz | \
		$(TAR) --strip-components=1 -C $(@D)/third_party/tsn-c-stack $(TAR_OPTIONS) -
	$(call suitable-extractor,$(MILAN_FPGA_SRC_LWSRP_REV).tar.gz) \
		$(MILAN_FPGA_SRC_DL_DIR)/$(MILAN_FPGA_SRC_LWSRP_REV).tar.gz | \
		$(TAR) --strip-components=1 -C $(@D)/third_party/lwSRP $(TAR_OPTIONS) -
endef
MILAN_FPGA_SRC_POST_EXTRACT_HOOKS += MILAN_FPGA_SRC_EXTRACT_SUBMODULES

$(eval $(generic-package))
