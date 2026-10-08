################################################################################
#
# milan-bridge
#
################################################################################

MILAN_BRIDGE_SITE = $(BR2_EXTERNAL_KL_AM62X_PATH)/../milan-linux
MILAN_BRIDGE_SITE_METHOD = local
MILAN_BRIDGE_LICENSE = Apache-2.0, CERN-OHL-W-2.0 (milan-fpga firmware)
MILAN_BRIDGE_DEPENDENCIES = milan-fpga-src alsa-lib
# the host build's objects and its milan-fpga link have no place here
MILAN_BRIDGE_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = --exclude build --exclude .milan-fpga

MILAN_BRIDGE_MAKE_OPTS = \
	CC="$(TARGET_CC)" \
	CFLAGS="$(TARGET_CFLAGS)" \
	LDFLAGS="$(TARGET_LDFLAGS)" \
	MILAN_FPGA=$(MILAN_FPGA_SRC_DIR) \
	O=$(@D)/out

define MILAN_BRIDGE_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) $(MILAN_BRIDGE_MAKE_OPTS) all
endef

define MILAN_BRIDGE_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) $(MILAN_BRIDGE_MAKE_OPTS) \
		DESTDIR=$(TARGET_DIR) PREFIX=/usr install
	$(INSTALL) -D -m 0755 $(@D)/scripts/milan-bridge.sh $(TARGET_DIR)/usr/sbin/milan-bridge.sh
	$(INSTALL) -D -m 0644 $(@D)/config/entity.conf $(TARGET_DIR)/etc/milan/entity.conf
endef

$(eval $(generic-package))
