// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// talker.c - see talker.h.

#define _GNU_SOURCE
#include "talker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "aaf.h"
#include "servo.h"

#define SLOT_NS 125000                  // one PDU every 125 us: 6 frames at 48 kHz
#define SERVO_SLOTS 400u                // a servo update every 50 ms
#define DP_SLOTS 80u                    // the datapath block is read every 10 ms
#define IDLE_SLOTS 80u                  // 10 ms without a frame: the host stopped
#define PRIME_WAIT_NS 20000000          // a start waits this long for the first frames
#define RESTART_LATE_NS 50000000        // this far behind its slots, a stream starts over
#define RESTART_EARLY_NS 50000000       // this far ahead too: the time base jumped back
#define MAX_CHANNELS 64u
#define MAX_FRAMES 64u

enum tstate { T_PRIMING, T_RUNNING, T_IDLE };

struct stream {
	struct aaf_params aaf;
	uint64_t dest_mac;
};

static void mac_bytes(uint8_t out[6], uint64_t mac)
{
	for (int i = 5; i >= 0; --i) {
		out[i] = (uint8_t)mac;
		mac >>= 8;
	}
}

// The stream milan-ctrld publishes for the source, if it has a destination.
static bool stream_of(const struct talker *tk, struct stream *s)
{
	struct milan_dp dp;
	if (!milan_dp_snapshot(tk->cfg.dp, &dp) || tk->cfg.source >= dp.n_sources) {
		return false;
	}
	const struct milan_dp_source *src = &dp.sources[tk->cfg.source];
	if (!src->dest_mac_valid) {
		return false;
	}
	memset(s, 0, sizeof *s);
	s->aaf.stream_id = src->stream_id;
	// tagged here, not by a VLAN device, so the frame leaves exactly as built
	s->aaf.vlan_tci = (uint16_t)(((unsigned)tk->cfg.priority & 7u) << 13 | (src->vlan_id & 0x0FFFu));
	mac_bytes(s->aaf.dst, src->dest_mac);
	mac_bytes(s->aaf.src, dp.mac);
	s->aaf.channels = tk->cfg.channels;
	s->aaf.frames = tk->cfg.frames_per_pdu;
	s->dest_mac = src->dest_mac;
	return true;
}

static int open_socket(const struct talker_cfg *cfg)
{
	int fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -errno;
	}
	// protocol 0: this socket sends and never receives
	struct sockaddr_ll sll = {.sll_family = AF_PACKET, .sll_ifindex = (int)if_nametoindex(cfg->ifname)};
	if (sll.sll_ifindex == 0 || bind(fd, (struct sockaddr *)&sll, sizeof sll) != 0 ||
	    setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &cfg->priority, sizeof cfg->priority) != 0) {
		int err = -errno;
		close(fd);
		return err ? err : -ENODEV;
	}
	return fd;
}

static void live_update(struct talker *tk, const struct milan_media_talker *t)
{
	shmblk_begin(&tk->live);
	tk->live.t = *t;
	shmblk_end(&tk->live);
}

void talker_report(const struct talker *tk, struct milan_media_talker *out)
{
	struct talker_live copy;
	if (shmblk_snapshot(&tk->live, &copy, sizeof copy)) {
		*out = copy.t;
	}
}

// Read and drop `frames` frames.
static void discard(struct audio_src *src, long frames)
{
	int32_t scratch[MAX_FRAMES * MAX_CHANNELS];
	while (frames > 0) {
		long n = frames > (long)MAX_FRAMES ? (long)MAX_FRAMES : frames;
		if (src->read(src, scratch, (unsigned)n) <= 0) {
			return;
		}
		frames -= n;
	}
}

// One stream, from its start to its stop (the destination changed or went away,
// the talker was told to stop, or it fell too far behind).
static void run_stream(struct talker *tk, int fd, const struct stream *s, struct milan_media_talker *rep)
{
	const struct talker_cfg *c = &tk->cfg;
	struct audio_src *src = c->src;
	struct servo servo;
	servo_init(&servo, c->level_target, (double)src->pitch_min - 1e6, (double)src->pitch_max - 1e6);
	src->set_pitch(src, 1000000);
	rep->pitch = 1000000;

	// start on a buffer that holds level_target frames, or on silence if the
	// host is not sending
	long a = src->avail(src);
	discard(src, a > 0 ? a : 0);

	// the priming wait runs on CLOCK_MONOTONIC, which a gPTP step cannot stretch
	struct timespec m;
	clock_gettime(CLOCK_MONOTONIC, &m);
	int64_t deadline = ts_ns(&m) + PRIME_WAIT_NS;

	while (src->avail(src) < (long)c->level_target && !atomic_load(&tk->stop)) {
		clock_gettime(CLOCK_MONOTONIC, &m);
		if (ts_ns(&m) >= deadline) {
			break;
		}

		struct timespec ts = {0, 50000};
		nanosleep(&ts, NULL);
	}

	int64_t t0 = ((gptp_now(c->clk) + SLOT_NS) / SLOT_NS + 1) * SLOT_NS;
	enum tstate state = src->avail(src) >= (long)c->level_target ? T_RUNNING : T_IDLE;

	rep->active = 1;
	rep->stream_id = s->aaf.stream_id;
	rep->dest_mac = s->dest_mac;
	rep->t0 = (uint64_t)t0;
	rep->stream_starts++;
	rep->level_min = INT32_MAX;
	rep->level_max = INT32_MIN;

	int32_t samples[MAX_FRAMES * MAX_CHANNELS];
	uint8_t frame[1600];
	uint8_t seq = 0;

	unsigned empty = 0;             // slots in a row with no new frame from the host
	uint64_t taken = 0;             // frames read from the source in this stream
	uint64_t seen_total = 0;        // frames the source had delivered at the last slot

	double level_sum = 0;
	unsigned level_n = 0;
	int32_t late_max = 0;

	for (uint64_t k = 0; !atomic_load(&tk->stop); ++k) {
		int64_t t = t0 + (int64_t)k * SLOT_NS;

		// a gPTP step (ptp4l's first lock, a grandmaster change) moves every
		// slot at once: start the stream over on the new time base
		int64_t ahead = gptp_sleep_until(c->clk, t);
		if (ahead > RESTART_EARLY_NS) {
			break;
		}

		int64_t delay = gptp_now(c->clk) - t;
		if (delay > RESTART_LATE_NS) {
			break;
		}

		long level = src->avail(src);
		if (level < 0) {
			level = 0;
		}

		// a host that stopped can leave a few frames, fewer than a PDU: what
		// tells it is that nothing new came in
		uint64_t total = taken + (uint64_t)level;
		if (total == seen_total) {
			empty++;
		} else {
			empty = 0;
		}
		seen_total = total;

		// the frames that came in after the slot, because this wake was late, are
		// not an excess: judge, and steer, the level the slot itself saw
		long lag = delay > 0 ? (long)(delay * 48 / 1000000) : 0;
		long seen = level > lag ? level - lag : 0;

		if (seen > (long)c->level_max) {
			long excess = seen - (long)c->level_target;

			discard(src, excess);
			taken += (uint64_t)excess;
			rep->overruns += (uint64_t)excess;

			level -= excess;
			seen = c->level_target;
		}

		bool have = false;

		switch (state) {
		case T_PRIMING:
			if (level >= (long)c->level_target) {
				state = T_RUNNING;
			}
			break;

		case T_RUNNING:
			if (level >= (long)c->frames_per_pdu) {
				long got = src->read(src, samples, c->frames_per_pdu);

				have = got == (long)c->frames_per_pdu;
				if (have) {
					taken += c->frames_per_pdu;
				}
			}

			if (empty >= IDLE_SLOTS) {
				state = T_IDLE;

				// what the host left goes, so a restart primes on fresh frames
				long left = src->avail(src);
				if (left > 0) {
					discard(src, left);
					taken += (uint64_t)left;
				}

				src->set_pitch(src, 1000000);
				servo_init(&servo, c->level_target, servo.lo, servo.hi);
				rep->pitch = 1000000;
			}
			break;

		case T_IDLE:
			if (empty == 0 && level > 0) {
				state = T_PRIMING;
			}
			break;
		}

		if (!have) {
			memset(samples, 0, sizeof(int32_t) * c->frames_per_pdu * c->channels);

			// silence counts as an underrun only while the host is sending
			if (state == T_RUNNING) {
				rep->underruns++;
			}
		}

		size_t n = aaf_build(frame, sizeof frame, &s->aaf, seq++, true, (uint32_t)(t + c->pto_ns), samples);
		if (send(fd, frame, n, MSG_DONTWAIT) == (ssize_t)n) {
			rep->frames_tx++;
		} else {
			rep->send_errors++;
		}
		if (delay > c->late_ns) {
			rep->late++;
		}
		if (delay > late_max) {
			late_max = delay > INT32_MAX ? INT32_MAX : (int32_t)delay;
		}
		if (state == T_RUNNING) {
			level_sum += (double)seen;
			level_n++;
			rep->level_min = seen < rep->level_min ? (int32_t)seen : rep->level_min;
			rep->level_max = seen > rep->level_max ? (int32_t)seen : rep->level_max;
		}

		if ((k + 1) % SERVO_SLOTS == 0u) {
			if (state == T_RUNNING && level_n > 0) {
				double out = servo_step(&servo, level_sum / level_n, (double)level_n * SLOT_NS * 1e-9);
				long pitch = 1000000 + (long)(out >= 0 ? out + 0.5 : out - 0.5);
				if (pitch != (long)rep->pitch) {
					src->set_pitch(src, pitch);
					rep->pitch = (uint32_t)pitch;
				}
				rep->level_avg_milli = (int32_t)(level_sum / level_n * 1000.0);
			}
			rep->locked = state == T_RUNNING && servo.locked;
			rep->late_max_ns = late_max;
			live_update(tk, rep);
			level_sum = 0;
			level_n = 0;
			late_max = 0;
			rep->level_min = INT32_MAX;
			rep->level_max = INT32_MIN;
		}
		if ((k + 1) % DP_SLOTS == 0u) {
			struct stream now;
			if (!stream_of(tk, &now) || now.aaf.stream_id != s->aaf.stream_id || now.dest_mac != s->dest_mac ||
			    now.aaf.vlan_tci != s->aaf.vlan_tci) {
				break;
			}
		}
	}
	rep->active = 0;
	rep->locked = 0;
	rep->stream_stops++;
	src->set_pitch(src, 1000000);
	rep->pitch = 1000000;
	live_update(tk, rep);
}

static void *talker_main(void *arg)
{
	struct talker *tk = arg;
	const struct talker_cfg *c = &tk->cfg;
	struct milan_media_talker rep;
	memset(&rep, 0, sizeof rep);
	rep.level_target = (int32_t)c->level_target;
	rep.pto_ns = c->pto_ns;
	rep.pitch = 1000000;
	live_update(tk, &rep);

	int fd = -1;
	while (!atomic_load(&tk->stop)) {
		struct stream s;
		if (fd < 0) {
			fd = open_socket(c);
		}
		if (fd < 0 || !stream_of(tk, &s)) {
			struct timespec ts = {0, 10000000};
			nanosleep(&ts, NULL);
			continue;
		}
		run_stream(tk, fd, &s, &rep);
	}
	if (fd >= 0) {
		close(fd);
	}
	return NULL;
}

int talker_start(struct talker *tk, const struct talker_cfg *cfg)
{
	memset(tk, 0, sizeof *tk);
	tk->cfg = *cfg;
	if (cfg->channels > MAX_CHANNELS || cfg->frames_per_pdu > MAX_FRAMES || cfg->level_target > cfg->level_max) {
		return EINVAL;
	}
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	// the default 8 MB stack would be locked whole under mlockall(MCL_FUTURE)
	pthread_attr_setstacksize(&attr, 256u * 1024u);
	if (cfg->rt_priority > 0) {
		struct sched_param sp = {.sched_priority = cfg->rt_priority};
		pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
		pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
		pthread_attr_setschedparam(&attr, &sp);
	}
	if (cfg->cpu >= 0) {
		cpu_set_t set;
		CPU_ZERO(&set);
		CPU_SET(cfg->cpu, &set);
		pthread_attr_setaffinity_np(&attr, sizeof set, &set);
	}
	int rc = pthread_create(&tk->thread, &attr, talker_main, tk);
	pthread_attr_destroy(&attr);
	return rc;
}

void talker_stop(struct talker *tk)
{
	atomic_store(&tk->stop, true);
	pthread_join(tk->thread, NULL);
}
