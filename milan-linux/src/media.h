// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// media.h - the media plane's status block, /dev/shm/milan-media: what
// milan-mediad reports about its streams (the Milan counters milan-ctrld will
// serve over AECP, and the numbers the validation reads). One writer
// (milan-mediad's main thread), the same sequence counter as datapath.h.

#ifndef MILAN_MEDIA_H
#define MILAN_MEDIA_H

#include <stdbool.h>
#include <stdint.h>

#define MILAN_MEDIA_NAME "/milan-media"
#define MILAN_MEDIA_MAGIC 0x3144454Du           // "MED1"
#define MILAN_MEDIA_VERSION 2u

struct milan_media_talker {
	uint8_t active;                 // a stream is being sent
	uint8_t locked;                 // the media clock servo is locked
	uint8_t pad[2];
	uint32_t pitch;                 // the last Capture Pitch written (1000000 = nominal)
	uint64_t stream_id;
	uint64_t dest_mac;
	uint64_t t0;                    // gPTP ns of PDU 0 of this stream
	uint64_t frames_tx;             // PDUs sent (Milan FRAMES_TX)
	uint64_t stream_starts;
	uint64_t stream_stops;
	uint64_t underruns;             // PDUs sent with silence: the host had no frames
	uint64_t overruns;              // frames dropped: the buffer ran past its maximum
	uint64_t late;                  // PDUs sent more than the late bound after their slot
	uint64_t send_errors;
	int32_t level_target;           // frames
	int32_t level_min;              // frames, over the last report interval
	int32_t level_max;
	int32_t level_avg_milli;        // frames x 1000
	int32_t late_max_ns;            // the largest send delay after a slot, last interval
	uint32_t pto_ns;
};

struct milan_media_listener {
	uint8_t active;                 // a stream is bound and arriving
	uint8_t locked;                 // MEDIA_LOCKED: presented on time, the servo locked
	uint8_t pad[2];
	uint32_t pitch;                 // the last Playback Pitch written
	uint64_t stream_id;
	uint64_t frames_rx;             // PDUs taken (Milan FRAMES_RX)
	uint64_t seq_mismatch;          // SEQ_NUM_MISMATCH
	uint64_t late_timestamp;        // LATE_TIMESTAMP: a PDU after its presentation time
	uint64_t early_timestamp;       // EARLY_TIMESTAMP: more than 4 x PTO ahead
	uint64_t unsupported_format;    // UNSUPPORTED_FORMAT
	uint64_t media_locked;          // MEDIA_LOCKED transitions
	uint64_t media_unlocked;        // MEDIA_UNLOCKED transitions
	uint64_t media_resets;          // MEDIA_RESET: a jump to put the stream back on time
	uint64_t stream_interrupted;    // STREAM_INTERRUPTED: no PDU for 10 ms
	uint64_t underruns;             // the gadget ran dry
	int32_t align_min_ns;           // presentation error, last report interval:
	int32_t align_max_ns;           // when a frame will reach USB minus its presentation time
	int32_t align_avg_ns;
	int32_t margin_min_ns;          // presentation time minus arrival, the least of the interval
	int64_t in_flight_ns;           // the USB IN pipeline the alignment accounts for
};

struct milan_media {
	uint32_t magic;
	uint32_t version;
	uint32_t seq;
	uint32_t writer_pid;
	int64_t gptp_corr_ns;           // gPTP - CLOCK_TAI
	int64_t gptp_residual_ns;       // its change at the last calibration
	uint8_t gptp_calibrated;
	uint8_t pad[7];
	struct milan_media_talker talker;
	struct milan_media_listener listener;
};

struct milan_media *milan_media_create(const char *name);
const struct milan_media *milan_media_open(const char *name);
void milan_media_begin(struct milan_media *m);
void milan_media_end(struct milan_media *m);
bool milan_media_snapshot(const struct milan_media *m, struct milan_media *out);

#endif // MILAN_MEDIA_H
