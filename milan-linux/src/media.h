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
#define MILAN_MEDIA_VERSION 3u

// ---- the Milan counters, as GET_COUNTERS serves them ----------------------------
//
// One stream's counters, each in the quadlet GET_COUNTERS puts it in (Milan
// v1.2 5.4.2.25: Table 5.16 for a STREAM_INPUT, Table 5.17 for a
// STREAM_OUTPUT), 32 bits that wrap. `valid` has bit k set for each quadlet k
// a media program counts; a stream no program receives or sends has none.
//
// Milan v1.2 defines most of them per observation interval (Table 5.4, Table
// 5.6): the counter adds one at the end of every interval during which its
// event happened at least once. The interval here is MILAN_OBS_NS of
// CLOCK_MONOTONIC, closed by its counting thread within 50 ms of its end, so
// none spans more than the 1 s Milan allows. MEDIA_LOCKED, MEDIA_UNLOCKED,
// STREAM_INTERRUPTED, STREAM_START and STREAM_STOP count each event.
//
// A STREAM_INPUT's are all zeroed each time the input goes from not bound to
// bound (Milan v1.2 5.3.8.10): `binds` is the datapath block's count of those
// for the sink (datapath.h) that the counters belong to. A STREAM_OUTPUT's
// MEDIA_RESET, TIMESTAMP_UNCERTAIN and FRAMES_TX are zeroed each time the
// talker starts streaming (Table 5.4).

#define MILAN_OBS_NS 500000000LL

// STREAM_INPUT quadlets (Milan v1.2 Table 5.16; IEEE 1722.1-2021 Table 7-157)
enum {
	MILAN_SI_MEDIA_LOCKED = 0, MILAN_SI_MEDIA_UNLOCKED = 1, MILAN_SI_STREAM_INTERRUPTED = 2,
	MILAN_SI_SEQ_NUM_MISMATCH = 3, MILAN_SI_MEDIA_RESET = 4, MILAN_SI_TIMESTAMP_UNCERTAIN = 5,
	MILAN_SI_UNSUPPORTED_FORMAT = 8, MILAN_SI_LATE_TIMESTAMP = 9, MILAN_SI_EARLY_TIMESTAMP = 10,
	MILAN_SI_FRAMES_RX = 11,
};
#define MILAN_SI_VALID 0xF3Fu                   // the ten of Table 5.16

// STREAM_OUTPUT quadlets (Milan v1.2 Table 5.17, which orders them unlike 1722.1)
enum {
	MILAN_SO_STREAM_START = 0, MILAN_SO_STREAM_STOP = 1, MILAN_SO_MEDIA_RESET = 2,
	MILAN_SO_TIMESTAMP_UNCERTAIN = 3, MILAN_SO_FRAMES_TX = 4,
};
#define MILAN_SO_VALID 0x1Fu                    // the five of Table 5.17

#define MILAN_COUNTERS 12u

struct milan_counters {
	uint32_t valid;
	uint32_t binds;                 // STREAM_INPUT: the sink's bind count these belong to
	uint32_t value[MILAN_COUNTERS];
};

// The counting side's interval, kept by the thread that counts (not shared).
struct milan_obs {
	uint32_t hits;                  // bit k: quadlet k's event happened in this interval
	int64_t end_ns;                 // CLOCK_MONOTONIC
};

int64_t milan_mono_ns(void);
// Start counting afresh at now: every value zeroed except `keep` (a mask of
// quadlets), the interval begun.
void milan_counters_zero(struct milan_counters *c, struct milan_obs *o, uint32_t keep, int64_t now_ns);
static inline void milan_obs_note(struct milan_obs *o, unsigned quadlet)
{
	o->hits |= 1u << quadlet;
}
// Close the interval if it has ended by now (or at once with `now_ns` <
// 0): one more in each counter whose event it saw. True if a counter moved.
bool milan_obs_close(struct milan_obs *o, struct milan_counters *c, int64_t now_ns);

// ---- each media program's own report ----------------------------------------------

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
	struct milan_counters milan;    // STREAM_OUTPUT 0
};

// The raw event counts below count every event (64 bits, never zeroed); the
// Milan counters in `milan` are what GET_COUNTERS serves.
struct milan_media_listener {
	uint8_t active;                 // a stream is bound and arriving
	uint8_t locked;                 // MEDIA_LOCKED: presented on time, the servo locked
	uint8_t pad[2];
	uint32_t pitch;                 // the last Playback Pitch written
	uint64_t stream_id;
	uint64_t frames_rx;             // PDUs taken
	uint64_t seq_mismatch;          // PDUs with a non-sequential sequence_num
	uint64_t late_timestamp;        // PDUs after their presentation time
	uint64_t early_timestamp;       // PDUs more than 4 x PTO ahead
	uint64_t unsupported_format;    // PDUs of another format than the input's
	uint64_t media_locked;          // MEDIA_LOCKED transitions
	uint64_t media_unlocked;        // MEDIA_UNLOCKED transitions
	uint64_t jumps;                 // the listener's own: a jump to put the stream back on time
	uint64_t stream_interrupted;    // STREAM_INTERRUPTED: no PDU for 10 ms, or a jump
	uint64_t timestamp_uncertain;   // PDUs with tu set
	uint64_t media_restarts;        // toggles of the mr bit
	uint64_t underruns;             // the gadget ran dry
	int32_t align_min_ns;           // presentation error, last report interval:
	int32_t align_max_ns;           // when a frame will reach USB minus its presentation time
	int32_t align_avg_ns;
	int32_t margin_min_ns;          // presentation time minus arrival, the least of the interval
	int64_t in_flight_ns;           // the USB IN pipeline the alignment accounts for
	struct milan_counters milan;    // STREAM_INPUT 0
};

// The CRF input's receive path (crf_rx.h): it counts the CRF stream bound to
// STREAM_INPUT 1 and tracks its media clock, which nothing follows.
struct milan_media_crf {
	uint8_t active;                 // a CRF stream is bound and arriving
	uint8_t locked;                 // its timestamps hold the nominal grid (MEDIA_LOCKED)
	uint8_t pad[6];
	uint64_t stream_id;
	uint64_t pdus;                  // PDUs of the stream received
	uint64_t seq_mismatch;
	uint64_t late_timestamp;
	uint64_t early_timestamp;
	uint64_t unsupported_format;
	uint64_t timestamp_uncertain;
	uint64_t media_restarts;        // toggles of the mr bit
	uint64_t media_locked;
	uint64_t media_unlocked;
	uint64_t stream_interrupted;
	uint64_t grid_breaks;           // timestamp steps off the nominal grid
	int64_t rate_ppb;               // its media clock against gPTP, over the current locked run
	int32_t step_err_max_ns;        // the largest step error of the last report interval
	int32_t margin_min_ns;          // timestamp minus arrival, the least of the interval
	struct milan_counters milan;    // STREAM_INPUT 1
};

struct milan_media {
	uint32_t magic;
	uint32_t version;
	uint32_t seq;
	uint32_t writer_pid;
	int64_t gptp_rate_ppb;          // the PHC's rate against CLOCK_MONOTONIC_RAW, less 1, in ppb
	int64_t gptp_residual_ns;       // the model's miss at the last measurement
	uint8_t gptp_calibrated;
	uint8_t pad[7];
	struct milan_media_talker talker;
	struct milan_media_listener listener;
	struct milan_media_crf crf;
};

struct milan_media *milan_media_create(const char *name);
const struct milan_media *milan_media_open(const char *name);
void milan_media_begin(struct milan_media *m);
void milan_media_end(struct milan_media *m);
bool milan_media_snapshot(const struct milan_media *m, struct milan_media *out);

#endif // MILAN_MEDIA_H
