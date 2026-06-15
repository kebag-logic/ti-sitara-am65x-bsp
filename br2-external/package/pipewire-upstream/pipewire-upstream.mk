################################################################################
#
# pipewire-upstream
#
################################################################################

# Official upstream PipeWire master (post-1.6.6), pinned for a reproducible build — bump to re-track master
PIPEWIRE_UPSTREAM_VERSION = bdf5b5a2a7f7acbf55241b5ecb74971daea3113f
PIPEWIRE_UPSTREAM_SITE = https://gitlab.freedesktop.org/pipewire/pipewire.git
PIPEWIRE_UPSTREAM_SITE_METHOD = git
PIPEWIRE_UPSTREAM_LICENSE = MIT
PIPEWIRE_UPSTREAM_LICENSE_FILES = COPYING
PIPEWIRE_UPSTREAM_INSTALL_STAGING = YES
PIPEWIRE_UPSTREAM_DEPENDENCIES = host-pkgconf alsa-lib dbus

# Same AVB/Milan-relevant options as the fork build; trim desktop bits for an embedded image
PIPEWIRE_UPSTREAM_CONF_OPTS = \
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
