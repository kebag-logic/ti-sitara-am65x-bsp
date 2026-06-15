#!/bin/bash

# SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic
# SPDX-License-Identifier: MIT

# Build a signed RAUC bundle (safe-update P2b) carrying a complete rootfs slot (rootfs + kernel + modules) as a tar image; RAUC formats the inactive ext4 slot and extracts it. On target: rauc install <bundle>.raucb
# Usage: build-rauc-bundle.sh [out.raucb]   needs res/rauc dev cert/key + a rauc binary (buildroot host-rauc or system rauc)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="${1:-$HERE/res/rauc/myir-am62x-bundle.raucb}"
ROOTTAR="${ROOTTAR:-$HERE/../buildroot/output/images/rootfs.tar.gz}"
KIMG="$HERE/linux/arch/arm64/boot/Image"
DTB="$HERE/res/spare-sd/boot/ti/k3-am625x-myd-6254-71.dtb"
MODDIR="${MODDIR:-$HERE/linux/output_modules/lib/modules}"
CERT="$HERE/res/rauc/rauc-dev.cert.pem"; KEY="$HERE/res/rauc/rauc-dev.key.pem"
VERSION="${VERSION:-$(date +%Y%m%d-%H%M%S)}"
# prefer the reproducible buildroot host tools (rauc + mksquashfs live here); fall back to a system rauc
HOSTBIN="$HERE/../buildroot/output/host/bin"; [ -d "$HOSTBIN" ] && export PATH="$HOSTBIN:$PATH"
RAUC="$HOSTBIN/rauc"; [ -x "$RAUC" ] || RAUC=$(command -v rauc || true)
[ -x "$RAUC" ] || { echo "no rauc binary; build it with: make -C ../buildroot host-rauc"; exit 1; }

for f in "$ROOTTAR" "$KIMG" "$DTB" "$CERT" "$KEY"; do [ -s "$f" ] || { echo "missing input: $f"; exit 1; }; done

WORK=$(mktemp -d); trap 'sudo rm -rf "$WORK"' EXIT
# assemble one complete slot tree (identical content to a build-spare-sd-ab.sh slot); sudo keeps root ownership/perms/device nodes
SLOT="$WORK/slot"; mkdir -p "$SLOT" "$WORK/content"
sudo tar -C "$SLOT" -xzf "$ROOTTAR"
sudo mkdir -p "$SLOT/boot"
sudo cp "$KIMG" "$SLOT/boot/Image"; sudo cp "$DTB" "$SLOT/boot/k3-am625x-myd-6254-71.dtb"
[ -d "$MODDIR" ] && sudo mkdir -p "$SLOT/lib/modules" && sudo cp -a "$MODDIR/." "$SLOT/lib/modules/"
sudo tar -C "$SLOT" -czf "$WORK/content/rootfs.tar.gz" .
sudo chown "$(id -u):$(id -g)" "$WORK/content/rootfs.tar.gz"

# manifest targets the 'rootfs' slot class (matches etc/rauc/system.conf slot.rootfs.{0,1}); plain = signed squashfs
cat > "$WORK/content/manifest.raucm" <<EOF
[update]
compatible=myir-am62x
version=$VERSION

[bundle]
format=plain

[image.rootfs]
filename=rootfs.tar.gz
EOF

rm -f "$OUT"
"$RAUC" bundle --cert="$CERT" --key="$KEY" "$WORK/content" "$OUT"
echo "built $OUT ($(du -h "$OUT"|cut -f1)) version=$VERSION"
echo "verify: $RAUC info $OUT   |   install on target: scp $OUT board:/tmp/ && ssh board rauc install /tmp/$(basename "$OUT")"
