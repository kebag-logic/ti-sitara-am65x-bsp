#!/bin/sh

# Tear down the composite gadget: unbind the UDC (drops usb0), then remove links + dirs in reverse
G=/sys/kernel/config/usb_gadget/g
[ -d "$G" ] || exit 0
echo "" > "$G/UDC" 2>/dev/null || true
for l in "$G"/configs/*/*; do [ -L "$l" ] && rm -f "$l"; done
rmdir "$G"/configs/*/strings/* "$G"/configs/* "$G"/functions/* "$G"/strings/* "$G" 2>/dev/null || true
