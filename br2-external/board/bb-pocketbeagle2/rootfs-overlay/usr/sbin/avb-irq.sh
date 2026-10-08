#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0

# Pin the CPSW (eth0) and USB interrupts of the PocketBeagle 2 to one core
# (issue #3). The "ethcap" label already keeps unmanaged IRQs off the isolated
# media-plane core (irqaffinity=0-2); this puts the two that carry the bridge's
# traffic together on AVB_IRQ_CPU, so CPUs 0 and 1 serve everything else. On
# PREEMPT_RT each handler runs in an irq/<n>-<name> thread, which follows its
# IRQ's affinity. Managed IRQs refuse the write; they are reported and left.
#
# usage: avb-irq.sh [status]

ENV_FILE=${AVB_ENV:-/etc/avb/avb.env}
if [ -r "$ENV_FILE" ]; then . "$ENV_FILE"; fi

CPU=${AVB_IRQ_CPU:-2}
MATCH=${AVB_IRQ_MATCH:-'8000000\.ethernet|31000000\.usb'}

# "<irq> <name>" for every numbered line of /proc/interrupts whose name matches;
# the pattern goes through the environment, since awk -v would eat its backslashes
irqs() {
	MATCH="$MATCH" awk '
		BEGIN { re = ENVIRON["MATCH"] }
		$1 ~ /^[0-9]+:$/ {
			n = $1; sub(/:$/, "", n)
			if ($NF ~ re) print n, $NF
		}' /proc/interrupts
}

if [ "${1:-pin}" = status ]; then
	irqs | while read -r n name; do
		echo "$n $name -> CPU $(cat "/proc/irq/$n/effective_affinity_list" 2>/dev/null)"
	done
	exit 0
fi

found=0
irqs | {
	while read -r n name; do
		found=1
		if echo "$CPU" > "/proc/irq/$n/smp_affinity_list" 2>/dev/null; then
			echo "avb-irq: $n $name -> CPU $CPU"
		else
			echo "avb-irq: $n $name left on CPU $(cat "/proc/irq/$n/effective_affinity_list" 2>/dev/null) (managed)"
		fi
	done
	[ "$found" -eq 1 ] || echo "avb-irq: no interrupt matches '$MATCH'"
}
