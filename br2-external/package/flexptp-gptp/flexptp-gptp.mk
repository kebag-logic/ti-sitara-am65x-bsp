################################################################################
#
# flexptp-gptp
#
################################################################################

# The revision: package/flexptp-gptp/flexptp.pin. For a local tree instead,
# set FLEXPTP_GPTP_OVERRIDE_SRCDIR in the output directory's local.mk.
FLEXPTP_GPTP_PIN = $(BR2_EXTERNAL_KL_AM62X_PATH)/package/flexptp-gptp/flexptp.pin
FLEXPTP_GPTP_VERSION = $(shell sed -n 's/^FLEXPTP_REV=//p' $(FLEXPTP_GPTP_PIN))
FLEXPTP_GPTP_SITE = https://github.com/kebag-logic/flexPTP/archive
FLEXPTP_GPTP_SOURCE = $(FLEXPTP_GPTP_VERSION).tar.gz
FLEXPTP_GPTP_LICENSE = MIT
FLEXPTP_GPTP_LICENSE_FILES = LICENSE.txt

# the daemon's CMake project, which builds the library from the tree above it
FLEXPTP_GPTP_SUBDIR = linux
FLEXPTP_GPTP_SUPPORTS_IN_SOURCE_BUILD = NO
FLEXPTP_GPTP_CONF_OPTS = -DCMAKE_BUILD_TYPE=Release

define FLEXPTP_GPTP_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/linux/buildroot-build/flexptpd $(TARGET_DIR)/usr/sbin/flexptpd
	$(INSTALL) -D -m 0755 $(@D)/linux/buildroot-build/flexptpd-status $(TARGET_DIR)/usr/bin/flexptpd-status
endef

$(eval $(cmake-package))
