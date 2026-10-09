# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0
#
# htop is the board's process viewer (bb_pocketbeagle2_avb_defconfig): `top`
# in an interactive shell runs it. Scripts are not affected, and BusyBox's own
# top is still `busybox top`.
if [ -n "$PS1" ] && command -v htop >/dev/null 2>&1; then
	alias top=htop
fi
