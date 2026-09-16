#!/bin/sh

# Buildroot post-build for the TI SK-AM62B-P1: make the init scripts + gadget
# helpers executable, lock down /root/.ssh, and bake the cross-built kernel
# modules into the rootfs so the produced image needs no post-flash scp.
#
# The executables come from two overlays - board/common/rootfs-overlay (the
# board-independent TDM8/gadget scripts) and this board's own - but Buildroot
# has already merged both into TARGET_DIR by the time this runs.
set -e
TARGET_DIR="$1"
HERE=$(cd "$(dirname "$0")" && pwd)
BSP=$(cd "$HERE/../../.." && pwd)          # .../ti-sitara-am65x-bsp

for f in etc/init.d/S99usb_gadgets \
	root/setup_gadgets.sh root/remove_usb.sh usr/sbin/tdm8-uac2.sh; do
	chmod 0755 "$TARGET_DIR/$f" 2>/dev/null || true
done
chmod 0644 "$TARGET_DIR/etc/tdm8/tdm8.env" 2>/dev/null || true

# sshd StrictModes: key-based root login needs 0700 dir + 0600 authorized_keys
chmod 0700 "$TARGET_DIR/root/.ssh" 2>/dev/null || true
chmod 0600 "$TARGET_DIR/root/.ssh/authorized_keys" 2>/dev/null || true

# Kernel modules. KL_MODDIR is a <...>/lib/modules directory holding one or more
# <kernelrelease> trees produced by `make modules_install INSTALL_MOD_PATH=...`.
# Default: the SK TDM8 kernel's staging dir, so `build-tdm8-uac2-sk.sh build`
# followed by a Buildroot `make` yields an image with the TDM8 stack already in
# it. Set KL_MODDIR=none to skip. Nothing fails if the directory is absent.
KL_MODDIR="${KL_MODDIR:-$BSP/.kstage-tdm8-sk/lib/modules}"
if [ "$KL_MODDIR" != none ] && [ -d "$KL_MODDIR" ]; then
	for rel in "$KL_MODDIR"/*; do
		[ -d "$rel" ] || continue
		name=$(basename "$rel")
		mkdir -p "$TARGET_DIR/lib/modules/$name"
		cp -a "$rel"/. "$TARGET_DIR/lib/modules/$name/"
		# these point into the build host's kernel tree; dangling on target
		rm -f "$TARGET_DIR/lib/modules/$name/build" \
		      "$TARGET_DIR/lib/modules/$name/source"
		echo "post-build: installed kernel modules for $name"
	done
fi
