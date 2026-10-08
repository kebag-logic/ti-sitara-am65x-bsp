// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// gptp_time.c - see gptp_time.h.

#define _GNU_SOURCE
#include "gptp_time.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/ethtool.h>
#include <linux/ptp_clock.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/timex.h>
#include <unistd.h>

#define CAL_SAMPLES 9

int gptp_time_open(struct gptp_time *g, const char *ifname)
{
	memset(g, 0, sizeof *g);
	g->phc_fd = -1;
	g->phc_index = -1;
	struct ethtool_ts_info info = {.cmd = ETHTOOL_GET_TS_INFO};
	struct ifreq ifr;
	memset(&ifr, 0, sizeof ifr);
	if (strlen(ifname) >= sizeof ifr.ifr_name) {
		return -ENAMETOOLONG;
	}
	strcpy(ifr.ifr_name, ifname);
	ifr.ifr_data = (char *)&info;
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -errno;
	}
	int rc = ioctl(fd, SIOCETHTOOL, &ifr);
	close(fd);
	if (rc != 0 || info.phc_index < 0) {
		return 0;                       // no PHC: gPTP time is CLOCK_TAI
	}
	char path[32];
	snprintf(path, sizeof path, "/dev/ptp%d", info.phc_index);
	g->phc_fd = open(path, O_RDONLY | O_CLOEXEC);
	if (g->phc_fd < 0) {
		return -errno;
	}
	g->phc_index = info.phc_index;
	return 0;
}

void gptp_time_close(struct gptp_time *g)
{
	if (g->phc_fd >= 0) {
		close(g->phc_fd);
	}
	g->phc_fd = -1;
}

int gptp_time_calibrate(struct gptp_time *g)
{
	if (g->phc_fd < 0) {
		return 0;
	}
	// sys (CLOCK_REALTIME) before, PHC, sys after; the narrowest window wins
	struct ptp_sys_offset_extended ext;
	memset(&ext, 0, sizeof ext);
	ext.n_samples = CAL_SAMPLES;
	if (ioctl(g->phc_fd, PTP_SYS_OFFSET_EXTENDED, &ext) != 0) {
		return -errno;
	}
	struct timex tx = {0};
	if (adjtimex(&tx) < 0) {
		return -errno;
	}
	int64_t best_spread = INT64_MAX;
	int64_t best = 0;
	for (unsigned i = 0; i < ext.n_samples; ++i) {
		int64_t before = (int64_t)ext.ts[i][0].sec * 1000000000 + ext.ts[i][0].nsec;
		int64_t phc = (int64_t)ext.ts[i][1].sec * 1000000000 + ext.ts[i][1].nsec;
		int64_t after = (int64_t)ext.ts[i][2].sec * 1000000000 + ext.ts[i][2].nsec;
		if (after - before < best_spread) {
			best_spread = after - before;
			// CLOCK_TAI = CLOCK_REALTIME + the kernel's TAI offset
			best = phc - (before + (after - before) / 2 + (int64_t)tx.tai * 1000000000);
		}
	}
	int64_t old = atomic_load_explicit(&g->corr_ns, memory_order_relaxed);
	g->residual_ns = g->calibrated ? best - old : 0;
	g->spread_ns = best_spread;
	atomic_store_explicit(&g->corr_ns, best, memory_order_relaxed);
	g->calibrated = true;
	return 0;
}

int64_t gptp_sleep_until(const struct gptp_time *g, int64_t t)
{
	struct timespec mono;
	clock_gettime(CLOCK_MONOTONIC, &mono);

	int64_t ahead = t - gptp_now(g);
	if (ahead <= 0) {
		return ahead;
	}

	struct timespec ts = ns_ts(ts_ns(&mono) + ahead);
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
		// a signal woke it early: sleep on to the same instant
	}

	return ahead;
}
