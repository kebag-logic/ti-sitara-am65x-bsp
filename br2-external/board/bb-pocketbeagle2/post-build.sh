#!/bin/sh

# Buildroot post-build for the BeagleBoard.org PocketBeagle 2: make the init
# scripts + gadget helpers executable, lock down /root/.ssh, and bake the
# cross-built kernel modules into the rootfs so the produced image needs no
# post-flash scp.
#
# The executables come from two overlays - board/common/rootfs-overlay (the
# board-independent TDM8/gadget scripts) and this board's own - but Buildroot
# has already merged both into TARGET_DIR by the time this runs.
set -e
TARGET_DIR="$1"
HERE=$(cd "$(dirname "$0")" && pwd)
BSP=$(cd "$HERE/../../.." && pwd)          # .../ti-sitara-am65x-bsp

for f in etc/init.d/S99usb_gadgets etc/init.d/S05growrootfs etc/init.d/S95avb \
	root/setup_gadgets.sh root/remove_usb.sh usr/sbin/tdm8-uac2.sh \
	usr/sbin/avb-gptp.sh usr/sbin/avb-irq.sh usr/sbin/avb-shaper.sh; do
	chmod 0755 "$TARGET_DIR/$f" 2>/dev/null || true
done
for f in etc/tdm8/tdm8.env etc/avb/avb.env etc/avb/gPTP.cfg etc/avb/uac2-milan.env; do
	chmod 0644 "$TARGET_DIR/$f" 2>/dev/null || true
done

# linuxptp installs S65ptp4l and S66phc2sys, which run ptp4l on eth0 from
# /etc/linuxptp.cfg: UDPv4, end-to-end, client only. That is not gPTP, and it
# would start before S95avb changes the CPSW TX channels (which takes eth0 down).
# S95avb starts ptp4l and phc2sys itself, with /etc/avb/gPTP.cfg, after the
# shaper (avb-gptp.sh), so the stock pair is removed.
rm -f "$TARGET_DIR/etc/init.d/S65ptp4l" "$TARGET_DIR/etc/init.d/S66phc2sys"

# sshd StrictModes: key-based root login needs 0700 dir + 0600 authorized_keys
chmod 0700 "$TARGET_DIR/root/.ssh" 2>/dev/null || true
chmod 0600 "$TARGET_DIR/root/.ssh/authorized_keys" 2>/dev/null || true

# ssh password login, as well as keys. root is the only account, and OpenSSH's
# default "PermitRootLogin prohibit-password" refuses its password even though
# PasswordAuthentication defaults to yes, so both are set explicitly. The
# password is BR2_TARGET_GENERIC_ROOT_PASSWD. sshd takes the first value it
# reads, so each stock "#Keyword ..." line is turned into the active one in
# place rather than appending a second one after it. Idempotent.
SSHD_CONFIG="$TARGET_DIR/etc/ssh/sshd_config"
if [ -f "$SSHD_CONFIG" ]; then
	for kv in "PermitRootLogin yes" "PasswordAuthentication yes"; do
		k=${kv%% *}
		if grep -qE "^#?[[:space:]]*$k[[:space:]]" "$SSHD_CONFIG"; then
			sed -i -E "0,/^#?[[:space:]]*$k[[:space:]].*/s//$kv/" "$SSHD_CONFIG"
		else
			echo "$kv" >> "$SSHD_CONFIG"
		fi
		grep -q "^$kv\$" "$SSHD_CONFIG" || { echo "post-build: could not set '$kv'" >&2; exit 1; }
	done
	echo "post-build: sshd allows password login (PermitRootLogin yes, PasswordAuthentication yes)"
fi

# Kernel modules. KL_MODDIR is a <...>/lib/modules directory holding one or more
# <kernelrelease> trees produced by `make modules_install INSTALL_MOD_PATH=...`.
# Default: the PocketBeagle 2 TDM8 kernel's staging dir, so
# `build-tdm8-uac2-pb2.sh build` followed by a Buildroot `make` yields an image
# with the TDM8 stack already in it. Set KL_MODDIR=none to skip. Nothing fails
# if the directory is absent.
KL_MODDIR="${KL_MODDIR:-$BSP/.kstage-tdm8-pb2/lib/modules}"
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
