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
// makes the gadget send faster. An error past 1 ms is put right by a jump:
// silence inserted, or frames dropped, which interrupts the playback
// (STREAM_INTERRUPTED, once for PDUs in a row that need a jump). A toggle of
// the PDUs' mr bit (a media clock restart, IEEE 1722-2016 4.4.4.3) places the
// stream again from that PDU; while tu is set (4.4.4.7), or tv clear, the PDUs
// are played as they come, without a jump or a servo step. A PDU stamped more
// than 4 x PTO ahead is too far in the future to process (EARLY_TIMESTAMP) and
// is dropped.
//
// The stream comes from milan-ctrld's datapath block: the sink milan-ctrld
// settled through ACMP. The Milan counters are Milan v1.2 5.3.8.10's (Table
// 5.6, media.h): MEDIA_RESET from the mr bit, TIMESTAMP_UNCERTAIN from tu,
// zeroed when the sink goes from not bound to bound.

#ifndef MILAN_LISTENER_H
#define MILAN_LISTENER_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include "audio_src.h"
#include "datapath.h"
#include "gptp_time.h"
#include "media.h"
#include "rxsock.h"
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
	struct rxsock_state rx;         // its receive socket's setup
	struct listener_live live;
};

int listener_start(struct listener *ls, const struct listener_cfg *cfg);
void listener_stop(struct listener *ls);
void listener_report(const struct listener *ls, struct milan_media_listener *out);

#endif // MILAN_LISTENER_H
