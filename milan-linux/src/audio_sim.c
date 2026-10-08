// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// audio_sim.c - a simulated USB host for the host tests (see audio_src.h).
//
// Frames arrive whole microframes at a time, on the host's bus clock, which
// runs host_ppm fast against CLOCK_TAI (the tests' gPTP time). The rate
// follows the pitch from the moment it is set, as the host follows the
// gadget's feedback. The content is the counting ramp, so a capture of the
// stream can be checked bit for bit.

#define _GNU_SOURCE
#include <math.h>
#include <stdlib.h>
#include <time.h>

#include "audio_src.h"
#include "gptp_time.h"

struct sim {
	struct audio_src src;
	double rate;
	double bus;                     // 1 + host_ppm * 1e-6
	long pitch;
	int64_t t_start;                // CLOCK_TAI ns of microframe 0 of the bus clock
	double frames_at_change;        // frames sent up to the last pitch change
	int64_t uframe_at_change;       // the microframe of that change
	uint64_t consumed;              // frames taken
	uint64_t ramp_n;                // the ramp counter of the next frame
	uint32_t ramp_wrap;             // N: frames before the ramp wraps
};

static int64_t tai_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_TAI, &ts);
	return ts_ns(&ts);
}

// The bus clock's microframe at `t`; the grid never moves, as a host's SOFs do not.
static int64_t uframe_at(const struct sim *s, int64_t t)
{
	return (int64_t)floor((double)(t - s->t_start) * s->bus / 125000.0);
}

// Frames the host has sent by `t`: 48 000 * pitch per second of bus time,
// delivered at each microframe boundary of the bus clock.
static double sent_by(const struct sim *s, int64_t t)
{
	return s->frames_at_change +
	       (double)(uframe_at(s, t) - s->uframe_at_change) * (s->rate * (double)s->pitch / 1e6) / 8000.0;
}

static long sim_avail(struct audio_src *src)
{
	struct sim *s = (struct sim *)src;
	double sent = floor(sent_by(s, tai_now()));
	return sent > (double)s->consumed ? (long)(sent - (double)s->consumed) : 0;
}

static long sim_read(struct audio_src *src, int32_t *buf, unsigned frames)
{
	struct sim *s = (struct sim *)src;
	for (unsigned f = 0; f < frames; ++f) {
		uint32_t n = (uint32_t)(s->ramp_n % s->ramp_wrap);
		for (unsigned c = 0; c < src->channels; ++c) {
			buf[f * src->channels + c] = (int32_t)(n * src->channels + c);
		}
		s->ramp_n++;
	}
	s->consumed += frames;
	return frames;
}

static int sim_set_pitch(struct audio_src *src, long pitch)
{
	struct sim *s = (struct sim *)src;
	if (pitch < src->pitch_min) {
		pitch = src->pitch_min;
	} else if (pitch > src->pitch_max) {
		pitch = src->pitch_max;
	}
	int64_t now = tai_now();
	s->frames_at_change = sent_by(s, now);
	s->uframe_at_change = uframe_at(s, now);
	s->pitch = pitch;
	return 0;
}

static void sim_close(struct audio_src *src)
{
	free(src);
}

struct audio_src *audio_sim_open(unsigned channels, unsigned rate, double host_ppm)
{
	struct sim *s = calloc(1, sizeof *s);
	if (s == NULL) {
		return NULL;
	}
	s->src = (struct audio_src){
		.avail = sim_avail,
		.read = sim_read,
		.set_pitch = sim_set_pitch,
		.close = sim_close,
		.channels = channels,
		.pitch_min = 750000,
		.pitch_max = 1005000,
	};
	s->rate = rate;
	s->bus = 1.0 + host_ppm * 1e-6;
	s->pitch = 1000000;
	s->t_start = tai_now();
	s->ramp_wrap = (uint32_t)((1ull << 32) / channels);
	return &s->src;
}
