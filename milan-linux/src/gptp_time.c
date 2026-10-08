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
#include <unistd.h>

#define CAL_SAMPLES 9

int gptp_time_open(struct gptp_time *g, const char *ifname)
{
	memset(g, 0, sizeof *g);
	g->phc_fd = -1;
	g->phc_index = -1;
	g->model.rate = 1.0;

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

// Publish a new model to the real-time path.
static void publish(struct gptp_time *g, int64_t mono, int64_t phc, double rate)
{
	uint32_t s = atomic_load_explicit(&g->seq, memory_order_relaxed);

	atomic_store_explicit(&g->seq, s + 1, memory_order_relaxed);
	atomic_thread_fence(memory_order_release);

	g->model.mono_ref = mono;
	g->model.phc_ref = phc;
	g->model.rate = rate;

	atomic_store_explicit(&g->seq, s + 2, memory_order_release);
}

void gptp_time_feed(struct gptp_time *g, int64_t mono, int64_t phc)
{
	// how far the model was off at this point
	if (g->calibrated) {
		g->residual_ns = phc - gptp_at(g, mono);
	} else {
		g->residual_ns = 0;
	}

	// a clock step: what came before says nothing about the rate now. Only
	// once a rate has been measured: until then the model only knows the PHC
	// within the oscillators' difference, tens of ppm
	bool rated = g->n_points >= 2;
	bool step = rated && (g->residual_ns > GPTP_STEP_NS || g->residual_ns < -GPTP_STEP_NS);
	if (step) {
		g->steps++;
		g->n_points = 0;
		g->head = 0;
	}

	g->points[g->head].mono = mono;
	g->points[g->head].phc = phc;
	g->head = (g->head + 1) % GPTP_RATE_WINDOW;
	if (g->n_points < GPTP_RATE_WINDOW) {
		g->n_points++;
	}

	// a least-squares line through the window, anchored at this point: its
	// slope is the rate, and the anchor carries less of one read's noise than
	// the point itself. The rate before, while there is only this point.
	double rate = g->model.rate;
	int64_t anchor = phc;

	if (g->n_points >= 2) {
		// x: seconds before this point; y: ns of PHC before it
		double sx = 0;
		double sy = 0;

		for (unsigned i = 0; i < g->n_points; ++i) {
			sx += (double)(g->points[i].mono - mono) / 1e9;
			sy += (double)(g->points[i].phc - phc);
		}

		double mx = sx / g->n_points;
		double my = sy / g->n_points;
		double sxx = 0;
		double sxy = 0;

		for (unsigned i = 0; i < g->n_points; ++i) {
			double dx = (double)(g->points[i].mono - mono) / 1e9 - mx;
			double dy = (double)(g->points[i].phc - phc) - my;

			sxx += dx * dx;
			sxy += dx * dy;
		}

		if (sxx > 0) {
			double slope = sxy / sxx;       // PHC ns per second of CLOCK_MONOTONIC_RAW

			rate = slope / 1e9;
			anchor = phc + (int64_t)(my - slope * mx);
		}
	}

	publish(g, mono, anchor, rate);
	g->calibrated = true;
}

int gptp_time_calibrate(struct gptp_time *g)
{
	if (g->phc_fd < 0) {
		return 0;
	}

	// CLOCK_MONOTONIC_RAW before, PHC, after; the narrowest window wins
	struct ptp_sys_offset_extended ext;
	memset(&ext, 0, sizeof ext);
	ext.n_samples = CAL_SAMPLES;
	ext.clockid = CLOCK_MONOTONIC_RAW;

	if (ioctl(g->phc_fd, PTP_SYS_OFFSET_EXTENDED, &ext) != 0) {
		return -errno;
	}

	int64_t best_spread = INT64_MAX;
	int64_t best_mono = 0;
	int64_t best_phc = 0;

	for (unsigned i = 0; i < ext.n_samples; ++i) {
		int64_t before = (int64_t)ext.ts[i][0].sec * 1000000000 + ext.ts[i][0].nsec;
		int64_t phc = (int64_t)ext.ts[i][1].sec * 1000000000 + ext.ts[i][1].nsec;
		int64_t after = (int64_t)ext.ts[i][2].sec * 1000000000 + ext.ts[i][2].nsec;

		if (after - before < best_spread) {
			best_spread = after - before;
			best_mono = before + (after - before) / 2;
			best_phc = phc;
		}
	}

	g->spread_ns = best_spread;
	gptp_time_feed(g, best_mono, best_phc);
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
