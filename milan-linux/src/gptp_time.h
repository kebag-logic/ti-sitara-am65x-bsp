// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// gptp_time.h - gPTP time for the media plane.
//
// AVTP timestamps are gPTP time, which is the PHC of the AVB interface (the
// CPTS on the PB2), steered by ptp4l. Reading the PHC is a system call into the
// CPTS, so the real-time path reads CLOCK_TAI instead (a vDSO read), which
// phc2sys keeps on the PHC, plus a correction this module measures once a
// second against the PHC itself
// (PTP_SYS_OFFSET_EXTENDED). The correction absorbs a kernel TAI offset that
// phc2sys did not set (37 s) and phc2sys's residual, and the residual it
// measures is the media clock's error, which the bridge reports.
//
// Without a PHC (a veth in the host tests) the correction stays 0: gPTP time
// is CLOCK_TAI.

#ifndef MILAN_GPTP_TIME_H
#define MILAN_GPTP_TIME_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

struct gptp_time {
	int phc_fd;                     // -1 without a PHC
	int phc_index;
	_Atomic int64_t corr_ns;        // gPTP - CLOCK_TAI
	int64_t residual_ns;            // the last calibration's change of the correction
	int64_t spread_ns;              // the last calibration's read window (its uncertainty)
	bool calibrated;
};

// Find the interface's PHC (ETHTOOL_GET_TS_INFO) and open it. 0, also when the
// interface has none; -errno when it has one that cannot be opened.
int gptp_time_open(struct gptp_time *g, const char *ifname);
void gptp_time_close(struct gptp_time *g);

// Measure the correction against the PHC; call about once a second, off the
// real-time path. 0, or -errno.
int gptp_time_calibrate(struct gptp_time *g);

static inline int64_t ts_ns(const struct timespec *ts)
{
	return (int64_t)ts->tv_sec * 1000000000 + ts->tv_nsec;
}

static inline struct timespec ns_ts(int64_t ns)
{
	struct timespec ts = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
	return ts;
}

// gPTP time now, in ns.
static inline int64_t gptp_now(const struct gptp_time *g)
{
	struct timespec ts;
	clock_gettime(CLOCK_TAI, &ts);
	return ts_ns(&ts) + atomic_load_explicit(&g->corr_ns, memory_order_relaxed);
}

// Sleep until gPTP time `t` (ns). The wait is measured in gPTP time now and
// slept on CLOCK_MONOTONIC, which no clock step moves: an absolute sleep on
// CLOCK_TAI would last days if phc2sys stepped the clock back during it, as it
// does when gPTP first locks. Returns how far ahead `t` was (ns, negative when
// already past), so a caller can tell a time base that jumped.
int64_t gptp_sleep_until(const struct gptp_time *g, int64_t t);

#endif // MILAN_GPTP_TIME_H
