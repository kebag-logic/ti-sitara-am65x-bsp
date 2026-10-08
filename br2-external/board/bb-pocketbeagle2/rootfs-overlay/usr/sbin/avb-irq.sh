#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0

# Pin the CPSW (eth0) and USB interrupts of the PocketBeagle 2 to one core
# (issue #3). The "ethcap" label already keeps unmanaged IRQs off the isolated
# media-plane core (irqaffinity=0-2); this puts the two that carry the bridge's
# traffic together on AVB_IRQ_CPU, so CPUs 0 and 1 serve everything else. On
# PREEMPT_RT each handler runs in an irq/<n>-<name> thread, which follows its
# IRQ's affinity (the kernel refuses sched_setaffinity on them). The CPSW's
# TX/RX channel interrupts come through the K3 interrupt aggregator (MSI-INTA),
# which refuses an affinity too, so their threads keep CPUs 0-3 in their mask.
# On the bench they run on CPUs 0 and 1 only: the isolated CPU 3 has a
# scheduling domain of its own (isolcpus=domain), which wakeups and real-time
# balancing do not reach. "status" shows where each thread may run.
#
# usage: avb-irq.sh [status]

ENV_FILE=${AVB_ENV:-/etc/avb/avb.env}
if [ -r "$ENV_FILE" ]; then . "$ENV_FILE"; fi

CPU=${AVB_IRQ_CPU:-2}
MATCH=${AVB_IRQ_MATCH:-'8000000\.ethernet|dwc3|xhci-hcd'}

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

# the PIDs of IRQ n's irq/<n>-... threads (the handler's, and a secondary one's)
pidof_irq() {
	for c in /proc/[0-9]*/comm; do
		name=$(cat "$c" 2>/dev/null)

		case "$name" in
		irq/"$1"-*)
			p=${c#/proc/}
			echo "${p%/comm}"
			;;
		esac
	done
}

if [ "${1:-pin}" = status ]; then
	irqs | while read -r n name; do
		irq_cpus=$(cat "/proc/irq/$n/effective_affinity_list" 2>/dev/null)

		# where each of its threads may run
		threads=""
		for t in $(pidof_irq "$n"); do
			cpus=$(sed -n 's/^Cpus_allowed_list:[[:space:]]*//p' "/proc/$t/status")
			threads="$threads $cpus"
		done

		echo "$n $name -> CPU $irq_cpus, threads on CPU${threads:- ?}"
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
			echo "avb-irq: $n $name refuses an affinity (interrupt aggregator), left as it is"
		fi
	done
	[ "$found" -eq 1 ] || echo "avb-irq: no interrupt matches '$MATCH'"
}
