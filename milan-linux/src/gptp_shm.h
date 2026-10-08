// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// gptp_shm.h - the gPTP status that flexptpd publishes in shared memory.
//
// flexptpd (kebag-logic/flexPTP, branch gptp) is the PB2's IEEE 802.1AS end
// station. It keeps a snapshot of its port in /dev/shm/<name> (by default
// "flexptpd.<interface>"): the grandmaster, asCapable, the link delay, the
// rate ratios, the servo's time error. The block is a seqlock: its writer
// makes `seq` odd while it copies, even again after; a reader that saw it
// change or odd reads again.
//
// The layout below mirrors the fork's linux/flexptpd_shm.h and
// src/flexptp/gptp_status.h (PtpGptpStatus, layout 1): the same fields, in
// the same order, with natural alignment. The size and a few offsets are
// checked at compile time; `layout` and `size` are checked at run time.

#ifndef MILAN_GPTP_SHM_H
#define MILAN_GPTP_SHM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GPTP_SHM_MAGIC "FLXPTPD"
#define GPTP_SHM_LAYOUT 1u

// PtpBmcaFsmState, as portState carries it
enum gptp_port_state {
	GPTP_PORT_INITIALIZING = 0,
	GPTP_PORT_LISTENING = 1,
	GPTP_PORT_PRE_MASTER = 2,
	GPTP_PORT_MASTER = 3,
	GPTP_PORT_SLAVE = 4,
	GPTP_PORT_PASSIVE = 5,
	GPTP_PORT_UNCALIBRATED = 6,
	GPTP_PORT_FAULTY = 7,
	GPTP_PORT_DISABLED = 8,
};

// PtpGptpStatus. Identities in host order: the 64-bit value whose big-endian
// bytes are the ClockIdentity on the wire.
struct gptp_status {
	uint32_t version;
	uint32_t updates;

	uint8_t port_state;
	uint8_t link_up;
	uint8_t as_capable;
	uint8_t is_measuring_delay;
	uint8_t domain;
	uint8_t gm_present;
	uint8_t locked;
	uint8_t r0;

	uint64_t own_identity;

	uint64_t gm_identity;          // 0 when none
	uint8_t gm_priority1;
	uint8_t gm_clock_class;
	uint8_t gm_clock_accuracy;
	uint8_t gm_priority2;
	uint16_t gm_variance;
	uint16_t steps_removed;
	uint16_t gm_time_base_indicator;
	uint16_t r1;
	uint32_t gm_changes;

	int64_t mean_link_delay_ns;
	double neighbor_rate_ratio;
	double rate_ratio;
	int64_t time_error_ns;
	double tuning_ppb;
	int8_t log_sync_interval;
	int8_t log_pdelay_interval;
	int8_t log_announce_interval;
	uint8_t r2;

	uint32_t sync_rx;
	uint32_t sync_timeouts;
	uint32_t announce_rx;
	uint32_t announce_timeouts;
	uint32_t pdelay_lost;
	uint32_t pdelay_multiple;
	uint32_t signaling_rx;
	uint32_t tx_timestamps_lost;

	uint8_t rsync_enabled;
	uint8_t rsync_domain;
	uint16_t r3;
	uint32_t rsync_interval_us;
	uint32_t rsync_sent;
};

_Static_assert(sizeof(struct gptp_status) == 136, "gptp_status: PtpGptpStatus layout 1 is 136 bytes");
_Static_assert(offsetof(struct gptp_status, gm_identity) == 24, "gptp_status: gmIdentity at 24");
_Static_assert(offsetof(struct gptp_status, mean_link_delay_ns) == 48, "gptp_status: meanLinkDelay_ns at 48");
_Static_assert(offsetof(struct gptp_status, sync_rx) == 92, "gptp_status: syncRx at 92");
_Static_assert(offsetof(struct gptp_status, rsync_sent) == 132, "gptp_status: rsyncSent at 132");

// The block in /dev/shm.
struct gptp_shm_block {
	char magic[8];                 // "FLXPTPD\0"
	uint32_t layout;               // GPTP_SHM_LAYOUT
	uint32_t seq;                  // odd while the writer copies
	uint32_t size;                 // sizeof(struct gptp_status)
	uint32_t r;
	struct gptp_status status;
};

struct gptp_shm {
	char name[64];                 // the shm name, without the leading '/'
	const struct gptp_shm_block *block;
	size_t len;
};

// Remember the name; the block is mapped on the first read that finds it,
// since flexptpd may start after us. 0, or -EINVAL for a name too long.
int gptp_shm_open(struct gptp_shm *g, const char *name);
void gptp_shm_close(struct gptp_shm *g);

// A consistent copy of the status. False while the block does not exist yet,
// has another layout, or the writer kept it busy for every retry.
bool gptp_shm_read(struct gptp_shm *g, struct gptp_status *out);

// The grandmaster, as the control plane announces it: the GM's identity, or
// ours while there is none (we are then our own time base), as ptp4l reports.
static inline uint64_t gptp_status_grandmaster(const struct gptp_status *s)
{
	return s->gm_identity != 0 ? s->gm_identity : s->own_identity;
}

#endif // MILAN_GPTP_SHM_H
