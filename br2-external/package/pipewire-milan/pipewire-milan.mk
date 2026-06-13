################################################################################
#
# pipewire-milan
#
################################################################################

# Pinned to the commit the pipewire-helper submodule uses (branch 5307-milan-avb-talker-stream-fix)
PIPEWIRE_MILAN_VERSION = 8966d626063136799ff5bdc9f6ced879feec8bcd
PIPEWIRE_MILAN_SITE = https://gitlab.freedesktop.org/Mister-M-alt/pipewire.git
PIPEWIRE_MILAN_SITE_METHOD = git
PIPEWIRE_MILAN_LICENSE = MIT
PIPEWIRE_MILAN_LICENSE_FILES = COPYING
PIPEWIRE_MILAN_INSTALL_STAGING = YES
PIPEWIRE_MILAN_DEPENDENCIES = host-pkgconf alsa-lib dbus

# Enable the AVB/Milan module; trim desktop bits for an embedded image
PIPEWIRE_MILAN_CONF_OPTS = \
	-Davb=enabled \
	-Dalsa=enabled \
	-Dpipewire-alsa=enabled \
	-Ddbus=enabled \
	-Dudev=enabled \
	-Dsystemd=disabled \
	-Dgstreamer=disabled \
	-Dman=disabled \
	-Dtests=disabled \
	-Dexamples=disabled

$(eval $(meson-package))
