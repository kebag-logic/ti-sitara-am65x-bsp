// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// listener.h - the AAF listener of the USB-to-Milan bridge (issue #12):
// Milan to the UAC2 gadget's playback PCM, so the host records the stream.
//
// A frame with presentation time T must reach the host's USB IN packet at T.
// When the listener queues a PDU at gPTP time t, its first frame leaves for the
// host at about t + (frames queued ahead of it) / 48 000 + the IN requests
// already filled (in_flight_ns, req_number x 125 us). The difference from T is
// the presentation error. The first PDU of a stream is placed by queueing
// silence until the error is nought. After that the servo (servo.h) holds it at
// nought through the gadget's "Playback Pitch 1000000": a stream running late
// makes the gadget send faster. An error past 1 ms is put right by a jump
// (MEDIA_RESET): silence inserted, or frames dropped.
//
// The stream comes from milan-ctrld's datapath block: the sink milan-ctrld
// settled through ACMP. The counters follow Milan v1.2 5.3.7 (STREAM_INPUT).

#ifndef MILAN_LISTENER_H
#define MILAN_LISTENER_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include "audio_src.h"
#include "datapath.h"
#include "gptp_time.h"
#include "media.h"
#include "shmblk.h"

struct listener_cfg {
	const char *ifname;             // the AVB interface (eth0)
	unsigned sink;                  // the STREAM_INPUT (0: the AAF stream)
	unsigned channels;
	uint32_t pto_ns;                // for EARLY_TIMESTAMP: more than 4 x PTO ahead
	int64_t in_flight_ns;           // the USB IN requests already filled
	int64_t interrupt_ns;           // no PDU for this long: STREAM_INTERRUPTED
	int rt_priority;
	int cpu;
	struct audio_sink *snk;
	struct gptp_time *clk;
	const struct milan_dp *dp;
};

struct listener_live {
	struct shmblk_hdr hdr;
	struct milan_media_listener l;
};

struct listener {
	struct listener_cfg cfg;
	pthread_t thread;
	atomic_bool stop;
	struct listener_live live;
};

int listener_start(struct listener *ls, const struct listener_cfg *cfg);
void listener_stop(struct listener *ls);
void listener_report(const struct listener *ls, struct milan_media_listener *out);

#endif // MILAN_LISTENER_H
