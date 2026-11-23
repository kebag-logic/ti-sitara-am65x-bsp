// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// listener.c - see listener.h.

#define _GNU_SOURCE
#include "listener.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/filter.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <sched.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "aaf.h"
#include "servo.h"

#define FS 48000
#define SERVO_NS 50000000               // a servo update every 50 ms
#define CHECK_NS 10000000               // the datapath block is read every 10 ms
#define RESET_NS 1000000                // a presentation error past 1 ms is jumped, not steered
#define MAX_CHANNELS 64u
#define MAX_FRAMES 64u

enum lstate { L_NONE, L_WAITING, L_RUNNING };

struct lstream {
	uint64_t stream_id;
	uint64_t dest_mac;
};

// The sink in the datapath block: whether it listens, to what, and its bind
// count. False while the block cannot be read.
static bool sink_of(const struct listener *ls, struct milan_dp_sink *k)
{
	struct milan_dp dp;
	if (!milan_dp_snapshot(ls->cfg.dp, &dp) || ls->cfg.sink >= dp.n_sinks) {
		return false;
	}
	*k = dp.sinks[ls->cfg.sink];
	return true;
}

static void set_locked(struct milan_media_listener *r, bool locked)
{
	if (locked && !r->locked) {
		r->media_locked++;
		r->milan.value[MILAN_SI_MEDIA_LOCKED]++;
	} else if (!locked && r->locked) {
		r->media_unlocked++;
		r->milan.value[MILAN_SI_MEDIA_UNLOCKED]++;
	}
	r->locked = locked;
}

// playback interrupted for a reason other than an unbind (Milan v1.2 Table 5.6)
static void interrupted(struct milan_media_listener *r)
{
	r->stream_interrupted++;
	r->milan.value[MILAN_SI_STREAM_INTERRUPTED]++;
}

static void live_update(struct listener *ls, const struct milan_media_listener *l)
{
	shmblk_begin(&ls->live);
	ls->live.l = *l;
	shmblk_end(&ls->live);
}

void listener_report(const struct listener *ls, struct milan_media_listener *out)
{
	struct listener_live copy;
	if (shmblk_snapshot(&ls->live, &copy, sizeof copy)) {
		*out = copy.l;
	}
}

// The stream's frames only: its destination and AVTP subtype AAF, tagged (the
// tag still inline) or untagged (the kernel moved it to the packet's metadata).
static int set_filter(int fd, uint64_t mac)
{
	uint32_t hi = (uint32_t)(mac >> 16);
	uint32_t lo = (uint32_t)(mac & 0xFFFFu);
	// jump offsets count from the next instruction; 14 is the drop
	struct sock_filter f[] = {
		/* 0 */ BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
		/* 1 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, hi, 0, 12),
		/* 2 */ BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 4),
		/* 3 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, lo, 0, 10),
		/* 4 */ BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 12),
		/* 5 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x8100, 0, 4),
		/* 6 */ BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 16),
		/* 7 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AAF_ETHERTYPE, 0, 6),
		/* 8 */ BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 18),
		/* 9 */ BPF_JUMP(BPF_JMP | BPF_JA, 2, 0, 0),
		/* 10 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AAF_ETHERTYPE, 0, 3),
		/* 11 */ BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 14),
		/* 12 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AAF_SUBTYPE, 0, 1),
		/* 13 */ BPF_STMT(BPF_RET | BPF_K, 0xFFFF),
		/* 14 */ BPF_STMT(BPF_RET | BPF_K, 0),
	};
	struct sock_fprog prog = {sizeof f / sizeof f[0], f};
	return setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof prog);
}

static void membership(int fd, int ifindex, uint64_t mac, bool add)
{
	struct packet_mreq mr = {.mr_ifindex = ifindex, .mr_type = PACKET_MR_MULTICAST, .mr_alen = 6};
	for (int i = 5; i >= 0; --i) {
		mr.mr_address[i] = (uint8_t)mac;
		mac >>= 8;
	}
	(void)setsockopt(fd, SOL_PACKET, add ? PACKET_ADD_MEMBERSHIP : PACKET_DROP_MEMBERSHIP, &mr, sizeof mr);
}

static void write_silence(struct audio_sink *snk, long frames)
{
	static const int32_t zero[MAX_FRAMES * MAX_CHANNELS];
	while (frames > 0) {
		long n = frames > (long)MAX_FRAMES ? (long)MAX_FRAMES : frames;
		if (snk->write(snk, zero, (unsigned)n) <= 0) {
			return;
		}
		frames -= n;
	}
}

static void *listener_main(void *arg)
{
	struct listener *ls = arg;
	const struct listener_cfg *c = &ls->cfg;
	struct audio_sink *snk = c->snk;
	struct milan_media_listener rep;
	memset(&rep, 0, sizeof rep);
	rep.pitch = 1000000;
	rep.in_flight_ns = c->in_flight_ns;
	rep.milan.valid = MILAN_SI_VALID;       // this thread counts all ten (media.h)
	struct milan_obs obs;
	milan_counters_zero(&rep.milan, &obs, 0, milan_mono_ns());
	live_update(ls, &rep);

	int ifindex = (int)if_nametoindex(c->ifname);
	int fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
	struct sockaddr_ll sll = {.sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL), .sll_ifindex = ifindex};
	struct timeval tv = {0, 5000};
	if (fd < 0 || ifindex == 0 || set_filter(fd, 0) != 0 || bind(fd, (struct sockaddr *)&sll, sizeof sll) != 0 ||
	    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0) {
		return NULL;
	}
	int one = 1;
	(void)setsockopt(fd, SOL_PACKET, PACKET_IGNORE_OUTGOING, &one, sizeof one);
	// room for half a second of PDUs, should the thread be held off its core
	int rcvbuf = 1 << 20;
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);

	enum lstate state = L_NONE;
	struct lstream cur = {0};
	struct servo servo;
	servo_init(&servo, 0.0, (double)snk->pitch_min - 1e6, (double)snk->pitch_max - 1e6);
	uint8_t last_seq = 0;
	bool have_seq = false;
	bool last_mr = false, have_mr = false;
	bool jumping = false;           // the last timed PDU was jumped: one interruption, however many
	int64_t last_rx = 0, next_check = 0, next_servo = 0;
	double err_sum = 0;
	unsigned err_n = 0;
	int32_t samples[MAX_FRAMES * MAX_CHANNELS];
	uint8_t frame[2048];

	while (!atomic_load(&ls->stop)) {
		if (milan_obs_close(&obs, &rep.milan, milan_mono_ns())) {
			live_update(ls, &rep);
		}
		int64_t now = gptp_now(c->clk);
		if (now >= next_check) {
			next_check = now + CHECK_NS;
			struct milan_dp_sink k;
			bool read = sink_of(ls, &k);
			bool listening = read && k.listening;
			// a binding made since the counters were zeroed: they start again
			// for it (Milan v1.2 5.3.8.10), from a stream started afresh
			bool rebound = read && k.binds != rep.milan.binds;
			if (rebound || !listening || k.stream_id != cur.stream_id || k.dest_mac != cur.dest_mac) {
				if (state != L_NONE) {
					membership(fd, ifindex, cur.dest_mac, false);
					snk->stop(snk);
					set_locked(&rep, false);
					rep.active = 0;
				}
				if (rebound) {
					milan_counters_zero(&rep.milan, &obs, 0, milan_mono_ns());
					rep.milan.binds = k.binds;
				}
				state = L_NONE;
				memset(&cur, 0, sizeof cur);
				set_filter(fd, 0);
				if (listening) {
					cur = (struct lstream){k.stream_id, k.dest_mac};
					set_filter(fd, k.dest_mac);
					membership(fd, ifindex, k.dest_mac, true);
					rep.stream_id = k.stream_id;
					state = L_WAITING;
					have_seq = false;
					have_mr = false;
				}
				live_update(ls, &rep);
			}
		}

		ssize_t n = recv(fd, frame, sizeof frame, 0);
		now = gptp_now(c->clk);
		if (n <= 0) {
			// a timeout can race PDUs that came while this thread waited to run;
			// with any waiting, the stream was never interrupted
			uint8_t peek;
			bool pending = recv(fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT) > 0;
			if (state == L_RUNNING && !pending && now - last_rx > c->interrupt_ns) {
				interrupted(&rep);
				set_locked(&rep, false);
				rep.active = 0;
				snk->stop(snk);
				state = L_WAITING;
				have_seq = false;
				live_update(ls, &rep);
			}
			continue;
		}
		struct aaf_pdu pdu;
		if (state == L_NONE || !aaf_parse(frame, (size_t)n, &pdu) || pdu.stream_id != cur.stream_id) {
			continue;
		}
		// every PDU of the stream is received, whatever its format (Table 5.6
		// FRAMES_RX), and its header read: sequence, mr, tu, timestamp
		milan_obs_note(&obs, MILAN_SI_FRAMES_RX);
		if (have_seq && pdu.seq != (uint8_t)(last_seq + 1u)) {
			rep.seq_mismatch++;
			milan_obs_note(&obs, MILAN_SI_SEQ_NUM_MISMATCH);
		}
		last_seq = pdu.seq;
		have_seq = true;
		// IEEE 1722-2016 4.4.4.3: a toggle of mr is a media clock restart
		bool restart = have_mr && pdu.mr != last_mr;
		last_mr = pdu.mr;
		have_mr = true;
		if (restart) {
			rep.media_restarts++;
			milan_obs_note(&obs, MILAN_SI_MEDIA_RESET);
		}
		if (pdu.tu) {
			rep.timestamp_uncertain++;
			milan_obs_note(&obs, MILAN_SI_TIMESTAMP_UNCERTAIN);
		}
		// the presentation time: the 64-bit gPTP time nearest the 32-bit stamp,
		// judged only when tv says the stamp is valid (4.4.4.5)
		int64_t t_pres = now + (int32_t)(pdu.avtp_timestamp - (uint32_t)now);
		int64_t margin = t_pres - now;
		bool early = pdu.tv && margin > 4 * (int64_t)c->pto_ns;
		if (pdu.tv && margin < 0) {
			rep.late_timestamp++;
			milan_obs_note(&obs, MILAN_SI_LATE_TIMESTAMP);
		} else if (early) {
			rep.early_timestamp++;
			milan_obs_note(&obs, MILAN_SI_EARLY_TIMESTAMP);
		}
		size_t frames = pdu.data_len / (4u * c->channels);
		if (pdu.format != AAF_FORMAT_INT_32BIT || pdu.nsr != AAF_NSR_48KHZ || pdu.channels != c->channels ||
		    pdu.data_len % (4u * c->channels) != 0u || frames == 0u || frames > MAX_FRAMES) {
			rep.unsupported_format++;
			milan_obs_note(&obs, MILAN_SI_UNSUPPORTED_FORMAT);
			// nothing to play for interrupt_ns, though PDUs of another format arrive
			if (state == L_RUNNING && now - last_rx > c->interrupt_ns) {
				interrupted(&rep);
				set_locked(&rep, false);
				rep.active = 0;
				snk->stop(snk);
				state = L_WAITING;
				have_seq = false;
				live_update(ls, &rep);
			}
			continue;
		}
		if (early) {
			continue;               // too far in the future to process (Milan v1.2 Table 5.6): not played
		}
		rep.frames_rx++;
		last_rx = now;
		// a media clock restart: the stream is placed again, as after an
		// interruption (4.4.4.3: the Listener adjusts quickly to the new clock)
		if (restart && state == L_RUNNING) {
			set_locked(&rep, false);
			state = L_WAITING;
		}
		// a stamp to place and steer by: valid (tv), and not marked uncertain (tu)
		bool timed = pdu.tv && !pdu.tu;
		if (!pdu.tv && state == L_WAITING) {
			continue;               // nothing to place the stream's first frame by
		}
		if (pdu.tv) {
			rep.margin_min_ns = margin < rep.margin_min_ns ? (int32_t)margin : rep.margin_min_ns;
		}
		aaf_samples_to_host(samples, pdu.data, frames * c->channels);
		const int32_t *first = samples;

		if (state == L_WAITING) {
			// place the first frame at its presentation time
			jumping = false;
			snk->stop(snk);
			servo_init(&servo, 0.0, servo.lo, servo.hi);
			snk->set_pitch(snk, 1000000);
			rep.pitch = 1000000;
			int64_t lead = t_pres - now - c->in_flight_ns;
			write_silence(snk, lead > 0 ? lead * FS / 1000000000 : 0);
			snk->write(snk, first, (unsigned)frames);
			snk->start(snk);
			state = L_RUNNING;
			rep.active = 1;
			rep.margin_min_ns = INT32_MAX;
			rep.align_min_ns = INT32_MAX;
			rep.align_max_ns = INT32_MIN;
			next_servo = now + SERVO_NS;
			err_sum = 0;
			err_n = 0;
			continue;
		}

		// when this PDU's first frame will reach the host, against when it should;
		// a stamp that is not valid, or marked uncertain (it may not be gPTP
		// time, 4.4.4.7), is played as it comes and neither jumps nor steers
		int64_t err = now + (int64_t)snk->delay(snk) * 1000000000 / FS + c->in_flight_ns - t_pres;
		if (!timed) {
			// played as it comes
		} else if (err > RESET_NS) {
			// a jump puts the stream back on time: its playback interrupted, once
			// for the PDUs in a row that need one
			size_t drop = (size_t)(err * FS / 1000000000);
			rep.jumps++;
			if (!jumping) {
				interrupted(&rep);
			}
			jumping = true;
			if (drop >= frames) {
				continue;
			}
			first += drop * c->channels;
			frames -= drop;
		} else if (err < -RESET_NS) {
			rep.jumps++;
			if (!jumping) {
				interrupted(&rep);
			}
			jumping = true;
			write_silence(snk, -err * FS / 1000000000);
		} else {
			jumping = false;
			err_sum += (double)err;
			err_n++;
			rep.align_min_ns = err < rep.align_min_ns ? (int32_t)err : rep.align_min_ns;
			rep.align_max_ns = err > rep.align_max_ns ? (int32_t)err : rep.align_max_ns;
		}
		snk->write(snk, first, (unsigned)frames);

		if (now >= next_servo) {
			next_servo = now + SERVO_NS;
			if (err_n > 0) {
				double avg = err_sum / err_n;
				// late (positive) is too much queued: the gadget must send faster
				double out = servo_step(&servo, avg * FS / 1e9, SERVO_NS * 1e-9);
				long pitch = 1000000 - (long)(out >= 0 ? out + 0.5 : out - 0.5);
				if (pitch != (long)rep.pitch) {
					snk->set_pitch(snk, pitch);
					rep.pitch = (uint32_t)pitch;
				}
				rep.align_avg_ns = (int32_t)avg;
			}
			set_locked(&rep, servo.locked);
			rep.underruns = snk->underruns;
			live_update(ls, &rep);
			err_sum = 0;
			err_n = 0;
			rep.align_min_ns = INT32_MAX;
			rep.align_max_ns = INT32_MIN;
			rep.margin_min_ns = INT32_MAX;
		}
	}
	snk->stop(snk);
	close(fd);
	return NULL;
}

int listener_start(struct listener *ls, const struct listener_cfg *cfg)
{
	memset(ls, 0, sizeof *ls);
	ls->cfg = *cfg;
	if (cfg->channels > MAX_CHANNELS) {
		return EINVAL;
	}
	pthread_attr_t attr;
	pthread_attr_init(&attr);
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
	int rc = pthread_create(&ls->thread, &attr, listener_main, ls);
	pthread_attr_destroy(&attr);
	return rc;
}

void listener_stop(struct listener *ls)
{
	atomic_store(&ls->stop, true);
	pthread_join(ls->thread, NULL);
}
