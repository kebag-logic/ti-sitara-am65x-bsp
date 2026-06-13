#!/bin/sh

# Buildroot post-build: make the AVB init script executable in the target rootfs
set -e
TARGET_DIR="$1"
chmod 0755 "$TARGET_DIR/etc/init.d/S95avb" 2>/dev/null || true
