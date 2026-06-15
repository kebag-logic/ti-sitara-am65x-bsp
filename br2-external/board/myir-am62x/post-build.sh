#!/bin/sh

# Buildroot post-build: make the AVB + UAC2 gadget init scripts and helpers executable in the rootfs
set -e
TARGET_DIR="$1"
for f in etc/init.d/S95avb etc/init.d/S50uac2gadget etc/init.d/S99bootgood usr/sbin/setup-uac2-gadget.sh; do
	chmod 0755 "$TARGET_DIR/$f" 2>/dev/null || true
done
