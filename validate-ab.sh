#!/bin/bash

# SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic
# SPDX-License-Identifier: MIT

# Validate RAUC A<->B failover (safe-update P2b) against the board over ssh: show status, install a bundle to the inactive slot, reboot, confirm the boot slot switched, mark good. The previously-booted slot stays as the fallback so this is safe remotely. Run after an A/B card carrying the rauc rootfs is flashed and booted.
# Usage: validate-ab.sh [bundle.raucb]   env: BOARD (ssh host, default board)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
BOARD="${BOARD:-board}"
BUNDLE="${1:-$HERE/res/rauc/myir-am62x-bundle.raucb}"
SSH="ssh -o ConnectTimeout=10 $BOARD"
say(){ echo "== $* =="; }
boot_slot(){ $SSH "mount" | sed -n 's| on / type.*||p' | grep -o 'mmcblk1p[0-9]'; }

[ -s "$BUNDLE" ] || { echo "missing bundle $BUNDLE (run build-rauc-bundle.sh)"; exit 1; }
say "precheck: rauc present + current slot"
$SSH "command -v rauc >/dev/null" || { echo "board has no rauc; flash the A/B rauc rootfs first"; exit 1; }
BEFORE=$(boot_slot); say "currently booted from $BEFORE (p2=A p3=B)"
$SSH "rauc status" || true

say "install bundle -> writes the INACTIVE slot and points the bootchooser at it"
scp "$BUNDLE" "$BOARD:/tmp/upd.raucb"
$SSH "rauc install /tmp/upd.raucb"
$SSH "rauc status"

say "reboot and wait for the board"
$SSH "reboot" || true
sleep 8; for i in $(seq 1 60); do $SSH true 2>/dev/null && break; sleep 5; done

AFTER=$(boot_slot); say "after reboot booted from $AFTER"
[ "$AFTER" != "$BEFORE" ] || { echo "slot did NOT switch ($BEFORE) — inspect bootchooser env (fw_printenv BOOT_ORDER BOOT_A_LEFT BOOT_B_LEFT)"; exit 1; }
say "SLOT SWITCHED $BEFORE -> $AFTER : RAUC failover path OK"

say "mark the booted slot good (commit; otherwise next reboot rolls back to $BEFORE)"
$SSH "rauc status mark-good"
$SSH "rauc status"
say "PASS — A<->B update validated; $BEFORE remains as the fallback slot"
