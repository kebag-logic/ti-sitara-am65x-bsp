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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

// ---- the sink: a host taking the gadget's IN packets -------------------------

struct sim_sink {
	struct audio_sink snk;
	double rate;
	double bus;
	long pitch;
	int64_t t_start;
	int64_t uframe;                 // the last microframe the host took
	double owed;                    // fraction of a frame the next packet carries
	uint64_t queued;                // frames written, not yet taken
	bool running;
	FILE *wav;
	uint64_t wav_frames;
};

static int64_t sink_uframe(const struct sim_sink *k, int64_t t)
{
	return (int64_t)floor((double)(t - k->t_start) * k->bus / 125000.0);
}

// Every microframe since the last call takes its packet of frames: what is
// queued, or silence (an underrun) when nothing is.
static void sink_advance(struct sim_sink *k)
{
	int64_t now = sink_uframe(k, tai_now());
	if (!k->running) {
		k->uframe = now;
		return;
	}
	for (; k->uframe < now; ++k->uframe) {
		k->owed += k->rate * (double)k->pitch / 1e6 / 8000.0;
		uint64_t take = (uint64_t)k->owed;
		k->owed -= (double)take;
		if (take > k->queued) {
			k->snk.underruns++;
			take = k->queued;
		}
		k->queued -= take;
	}
}

static long sink_delay(struct audio_sink *snk)
{
	struct sim_sink *k = (struct sim_sink *)snk;
	sink_advance(k);
	return (long)k->queued;
}

static void put_le(FILE *f, uint32_t v, unsigned bytes)
{
	for (unsigned i = 0; i < bytes; ++i) {
		fputc((int)((v >> (8 * i)) & 0xFFu), f);
	}
}

static void wav_header(struct sim_sink *k)
{
	uint32_t data = (uint32_t)(k->wav_frames * k->snk.channels * 4u);
	fseek(k->wav, 0, SEEK_SET);
	fwrite("RIFF", 1, 4, k->wav);
	put_le(k->wav, 36u + data, 4);
	fwrite("WAVEfmt ", 1, 8, k->wav);
	put_le(k->wav, 16, 4);
	put_le(k->wav, 1, 2);                                   // PCM
	put_le(k->wav, k->snk.channels, 2);
	put_le(k->wav, (uint32_t)k->rate, 4);
	put_le(k->wav, (uint32_t)k->rate * k->snk.channels * 4u, 4);
	put_le(k->wav, k->snk.channels * 4u, 2);
	put_le(k->wav, 32, 2);
	fwrite("data", 1, 4, k->wav);
	put_le(k->wav, data, 4);
	fseek(k->wav, 0, SEEK_END);
}

static long sink_write(struct audio_sink *snk, const int32_t *buf, unsigned frames)
{
	struct sim_sink *k = (struct sim_sink *)snk;
	sink_advance(k);
	k->queued += frames;
	if (k->wav != NULL) {
		for (size_t i = 0; i < (size_t)frames * snk->channels; ++i) {
			put_le(k->wav, (uint32_t)buf[i], 4);
		}
		k->wav_frames += frames;
	}
	return frames;
}

static int sink_start(struct audio_sink *snk)
{
	struct sim_sink *k = (struct sim_sink *)snk;
	sink_advance(k);
	k->running = true;
	return 0;
}

static void sink_stop(struct audio_sink *snk)
{
	struct sim_sink *k = (struct sim_sink *)snk;
	sink_advance(k);
	k->running = false;
	k->queued = 0;
}

static int sink_set_pitch(struct audio_sink *snk, long pitch)
{
	struct sim_sink *k = (struct sim_sink *)snk;
	sink_advance(k);
	k->pitch = pitch < snk->pitch_min ? snk->pitch_min : pitch > snk->pitch_max ? snk->pitch_max : pitch;
	return 0;
}

static void sink_close(struct audio_sink *snk)
{
	struct sim_sink *k = (struct sim_sink *)snk;
	if (k->wav != NULL) {
		wav_header(k);
		fclose(k->wav);
	}
	free(k);
}

struct audio_sink *audio_sim_sink_open(unsigned channels, unsigned rate, double host_ppm, const char *wav)
{
	struct sim_sink *k = calloc(1, sizeof *k);
	if (k == NULL) {
		return NULL;
	}
	k->snk = (struct audio_sink){
		.delay = sink_delay,
		.write = sink_write,
		.start = sink_start,
		.stop = sink_stop,
		.set_pitch = sink_set_pitch,
		.close = sink_close,
		.channels = channels,
		.pitch_min = 750000,
		.pitch_max = 1005000,
	};
	k->rate = rate;
	k->bus = 1.0 + host_ppm * 1e-6;
	k->pitch = 1000000;
	k->t_start = tai_now();
	if (wav != NULL) {
		k->wav = fopen(wav, "w+b");
		if (k->wav == NULL) {
			free(k);
			return NULL;
		}
		wav_header(k);
	}
	return &k->snk;
}
