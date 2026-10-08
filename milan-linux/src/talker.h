// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// talker.h - the AAF talker of the USB-to-Milan bridge (issues #10 and #11).
//
// The talker runs on the media clock, not on the USB host's: PDU k of a stream
// leaves at gPTP time t_k = T0 + k * 125 us, carries 6 frames taken from the
// gadget's capture buffer, and is stamped t_k + PTO. Its timestamps therefore
// step by exactly 125 000 ns, whatever the host does. The servo (servo.h) steers
// the host, through the gadget's feedback, until the buffer holds
// level_target frames at each slot. That level is the bridge's share of the
// latency: a frame waits about level_target / 48 000 s between its USB packet
// and its PDU (24 frames = 500 us).
//
//   PRIMING  after a start or a pause: silence goes out, nothing is read,
//            until the buffer holds level_target frames
//   RUNNING  6 frames per PDU; the servo runs. A PDU with fewer than 6 frames
//            waiting goes out with silence (an underrun); frames past
//            level_max are dropped (an overrun)
//   IDLE     the host sent nothing for 10 ms: silence, pitch back to nominal,
//            and PRIMING again on the first frame
//
// The stream goes out while milan-ctrld publishes a MAAP destination for the
// source (the datapath block). Gating it on an SRP Listener Ready waits for
// SRP (#14).

#ifndef MILAN_TALKER_H
#define MILAN_TALKER_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "audio_src.h"
#include "datapath.h"
#include "gptp_time.h"
#include "media.h"
#include "shmblk.h"

struct talker_cfg {
	const char *ifname;             // where the PDUs go: the AVB interface, eth0
	int priority;                   // SO_PRIORITY, the class A traffic class, and the PCP (3)
	unsigned source;                // the STREAM_OUTPUT (0: the AAF stream)
	unsigned channels;
	unsigned frames_per_pdu;        // 6: class A at 48 kHz
	uint32_t pto_ns;                // presentation time offset
	unsigned level_target;          // frames
	unsigned level_max;             // frames
	int64_t late_ns;                // a PDU sent later than this after its slot is late
	int rt_priority;                // SCHED_FIFO of the talker thread
	int cpu;                        // its CPU, -1 for any
	struct audio_src *src;
	struct gptp_time *clk;
	const struct milan_dp *dp;
};

// What the talker thread reports, under a sequence counter of its own.
struct talker_live {
	struct shmblk_hdr hdr;
	struct milan_media_talker t;
};

struct talker {
	struct talker_cfg cfg;
	pthread_t thread;
	atomic_bool stop;
	struct talker_live live;
};

// Start the talker thread. 0, or an errno.
int talker_start(struct talker *tk, const struct talker_cfg *cfg);
void talker_stop(struct talker *tk);
// A consistent copy of what the talker reports.
void talker_report(const struct talker *tk, struct milan_media_talker *out);

#endif // MILAN_TALKER_H
