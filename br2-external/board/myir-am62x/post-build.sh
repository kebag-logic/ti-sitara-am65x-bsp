#!/bin/sh

# Buildroot post-build: make the init scripts + gadget/ssh helpers executable and lock down /root/.ssh
set -e
TARGET_DIR="$1"
# drop the stale audio-only gadget init (superseded by the composite S99usb_gadgets); target/ is incremental so overlay deletion alone won't remove it
rm -f "$TARGET_DIR/etc/init.d/S50uac2gadget"
for f in etc/init.d/S95avb etc/init.d/S99bootgood etc/init.d/S99usb_gadgets \
	root/setup_gadgets.sh root/remove_usb.sh usr/sbin/setup-uac2-gadget.sh; do
	chmod 0755 "$TARGET_DIR/$f" 2>/dev/null || true
done
# sshd StrictModes: key-based root login needs 0700 dir + 0600 authorized_keys
chmod 0700 "$TARGET_DIR/root/.ssh" 2>/dev/null || true
chmod 0600 "$TARGET_DIR/root/.ssh/authorized_keys" 2>/dev/null || true
