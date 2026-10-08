// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// gptp_time.h - gPTP time for the media plane.
//
// AVTP timestamps are gPTP time: the PHC of the AVB interface (the CPTS on the
// PB2), which flexptpd steers onto the grandmaster. Nothing steers the system
// clocks onto it (there is no phc2sys), and reading the PHC is a system call
// into the CPTS, too slow for every slot. So the real-time path reads
// CLOCK_MONOTONIC_RAW (a vDSO read) and maps it onto the PHC with a model
// measured once a second against the PHC itself (PTP_SYS_OFFSET_EXTENDED on
// CLOCK_MONOTONIC_RAW):
//
//     gPTP time = phc_ref + (CLOCK_MONOTONIC_RAW - mono_ref) * rate
//
// Each measurement re-anchors (phc_ref, mono_ref). The rate spans the latest
// measurements (a window of GPTP_RATE_WINDOW seconds), which averages out the
// read uncertainty and flexptpd's steering. A measurement the model missed by
// more than GPTP_STEP_NS is a clock step (flexptpd's first lock, a grandmaster
// change): the window starts over from it. The miss of each measurement, the
// residual, is the media clock's error, which the bridge reports.
//
// Without a PHC (a veth in the host tests) gPTP time is CLOCK_TAI.

#ifndef MILAN_GPTP_TIME_H
#define MILAN_GPTP_TIME_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define GPTP_RATE_WINDOW 16             // measurements the rate spans
#define GPTP_STEP_NS 20000              // a miss this large is a step, not drift

// The model the real-time path reads, under a seqlock.
struct gptp_model {
	int64_t mono_ref;               // CLOCK_MONOTONIC_RAW at the anchor (ns)
	int64_t phc_ref;                // the PHC at the anchor (ns)
	double rate;                    // PHC ns per CLOCK_MONOTONIC_RAW ns
};

struct gptp_point {
	int64_t mono;
	int64_t phc;
};

struct gptp_time {
	int phc_fd;                     // -1 without a PHC
	int phc_index;

	_Atomic uint32_t seq;           // odd while the model is written
	struct gptp_model model;

	// the calibrator's own state, off the real-time path
	struct gptp_point points[GPTP_RATE_WINDOW];
	unsigned n_points;
	unsigned head;
	int64_t residual_ns;            // the last measurement's miss
	int64_t spread_ns;              // its read window (its uncertainty)
	uint32_t steps;                 // steps detected
	bool calibrated;
};

// Find the interface's PHC (ETHTOOL_GET_TS_INFO) and open it. 0, also when the
// interface has none; -errno when it has one that cannot be opened.
int gptp_time_open(struct gptp_time *g, const char *ifname);
void gptp_time_close(struct gptp_time *g);

// Measure the PHC against CLOCK_MONOTONIC_RAW and update the model; call
// about once a second, off the real-time path. 0, or -errno.
int gptp_time_calibrate(struct gptp_time *g);

// Update the model from one measurement: the PHC read at mono. The part of
// gptp_time_calibrate() after the read, exposed for the tests.
void gptp_time_feed(struct gptp_time *g, int64_t mono, int64_t phc);

static inline int64_t ts_ns(const struct timespec *ts)
{
	return (int64_t)ts->tv_sec * 1000000000 + ts->tv_nsec;
}

static inline struct timespec ns_ts(int64_t ns)
{
	struct timespec ts = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
	return ts;
}

// The model's gPTP time at a CLOCK_MONOTONIC_RAW reading.
static inline int64_t gptp_at(const struct gptp_time *g, int64_t mono)
{
	struct gptp_model m;

	for (;;) {
		uint32_t before = atomic_load_explicit(&g->seq, memory_order_acquire);

		if (before & 1u) {
			continue;
		}

		m = g->model;
		atomic_thread_fence(memory_order_acquire);

		if (atomic_load_explicit(&g->seq, memory_order_relaxed) == before) {
			break;
		}
	}

	return m.phc_ref + (int64_t)((double)(mono - m.mono_ref) * m.rate);
}

// gPTP time now, in ns.
static inline int64_t gptp_now(const struct gptp_time *g)
{
	struct timespec ts;

	if (g->phc_fd < 0 || !g->calibrated) {
		clock_gettime(CLOCK_TAI, &ts);
		return ts_ns(&ts);
	}

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return gptp_at(g, ts_ns(&ts));
}

// Sleep until gPTP time `t` (ns). The wait is measured in gPTP time now and
// slept on CLOCK_MONOTONIC, which no clock step moves: an absolute sleep on a
// clock that gets stepped could last days. Returns how far ahead `t` was (ns,
// negative when already past), so a caller can tell a time base that jumped.
int64_t gptp_sleep_until(const struct gptp_time *g, int64_t t);

#endif // MILAN_GPTP_TIME_H
