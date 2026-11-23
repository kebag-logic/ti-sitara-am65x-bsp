// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// crf_rx.h - the CRF Media Clock Input's receive path (STREAM_INPUT 1, Milan
// v1.2 7.2.2): it receives the CRF stream milan-ctrld settled the sink on,
// tracks the media clock the stream carries, and counts it. Nothing on the
// board clocks from that media clock: milan-mediad's talker and listener
// keep their own gPTP-derived clock whatever CLOCK_SOURCE says
// (AECP-STATUS.md). What this path does is what the stream's counters need.
//
// A PDU of the bound stream is received (FRAMES_RX) whatever is in it. Its
// header is read as the AAF listener reads its own: a non-sequential
// sequence_num, a toggled mr (a media clock restart, IEEE 1722-2016 10.4.3), a
// set tu (10.4.5). A PDU in another format than the Milan CRF format the input
// runs (crf.h, Milan v1.2 Table 7.1) is UNSUPPORTED_FORMAT and goes no
// further. Each timestamp of a Milan PDU is a presentation time (10.4.13.1,
// 10.7: the talker's time plus the Max Transit Time): in the past on arrival it
// is LATE_TIMESTAMP, and still a point of the media clock (10.6: late stamps
// can recreate it); more than early_ns ahead it is EARLY_TIMESTAMP, too far in
// the future to process, and goes no further.
//
// "Synchronized on the media clock" (MEDIA_LOCKED, Milan v1.2 Table 5.6,
// whose definition is left to the implementation) is, here: the last
// LOCK_STEPS timestamps, PDU to PDU, each stepped by the nominal interval
// (timestamp_interval / base_frequency, 2 ms) times the sequence numbers
// between them, within STEP_TOL_NS. A step off that grid, a media clock
// restart, or no Milan PDU for interrupt_ns (STREAM_INTERRUPTED) ends the
// lock (MEDIA_UNLOCKED). While tu is set the steps are not judged, since the
// stamps may jump (10.4.5; 10.7, note 2): the grid is taken up again from the
// next stamp.
//
// The counters are zeroed each time the sink goes from not bound to bound
// (Milan v1.2 5.3.8.10).

#ifndef MILAN_CRF_RX_H
#define MILAN_CRF_RX_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include "datapath.h"
#include "gptp_time.h"
#include "media.h"
#include "shmblk.h"

#define CRF_RX_LOCK_STEPS 25u           // 50 ms of 2 ms steps on the grid
#define CRF_RX_STEP_TOL_NS 1000         // a step's tolerance: 500 ppm of 2 ms

struct crf_rx_cfg {
	const char *ifname;             // the AVB interface (eth0)
	unsigned sink;                  // the STREAM_INPUT (1: the CRF input)
	int64_t early_ns;               // a stamp further ahead than this is EARLY_TIMESTAMP
	int64_t interrupt_ns;           // no Milan PDU for this long: STREAM_INTERRUPTED
	int rt_priority;
	int cpu;
	struct gptp_time *clk;
	const struct milan_dp *dp;
};

struct crf_rx_live {
	struct shmblk_hdr hdr;
	struct milan_media_crf r;
};

struct crf_rx {
	struct crf_rx_cfg cfg;
	pthread_t thread;
	atomic_bool stop;
	struct crf_rx_live live;
};

int crf_rx_start(struct crf_rx *x, const struct crf_rx_cfg *cfg);
void crf_rx_stop(struct crf_rx *x);
void crf_rx_report(const struct crf_rx *x, struct milan_media_crf *out);

#endif // MILAN_CRF_RX_H
