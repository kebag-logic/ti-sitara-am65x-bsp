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
#define INTERRUPT_NS 10000000           // no PDU for 10 ms: STREAM_INTERRUPTED
#define RESET_NS 1000000                // a presentation error past 1 ms is jumped, not steered
#define MAX_CHANNELS 64u
#define MAX_FRAMES 64u

enum lstate { L_NONE, L_WAITING, L_RUNNING };

struct lstream {
	uint64_t stream_id;
	uint64_t dest_mac;
};

static bool stream_of(const struct listener *ls, struct lstream *s)
{
	struct milan_dp dp;
	if (!milan_dp_snapshot(ls->cfg.dp, &dp) || ls->cfg.sink >= dp.n_sinks) {
		return false;
	}
	const struct milan_dp_sink *k = &dp.sinks[ls->cfg.sink];
	if (!k->listening) {
		return false;
	}
	s->stream_id = k->stream_id;
	s->dest_mac = k->dest_mac;
	return true;
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

	enum lstate state = L_NONE;
	struct lstream cur = {0};
	struct servo servo;
	servo_init(&servo, 0.0, (double)snk->pitch_min - 1e6, (double)snk->pitch_max - 1e6);
	uint8_t last_seq = 0;
	bool have_seq = false;
	int64_t last_rx = 0, next_check = 0, next_servo = 0;
	double err_sum = 0;
	unsigned err_n = 0;
	int32_t samples[MAX_FRAMES * MAX_CHANNELS];
	uint8_t frame[2048];

	while (!atomic_load(&ls->stop)) {
		int64_t now = gptp_now(c->clk);
		if (now >= next_check) {
			next_check = now + CHECK_NS;
			struct lstream s;
			bool listening = stream_of(ls, &s);
			if (!listening || s.stream_id != cur.stream_id || s.dest_mac != cur.dest_mac) {
				if (state != L_NONE) {
					membership(fd, ifindex, cur.dest_mac, false);
					snk->stop(snk);
					if (rep.locked) {
						rep.media_unlocked++;
					}
					rep.locked = 0;
					rep.active = 0;
				}
				state = L_NONE;
				memset(&cur, 0, sizeof cur);
				set_filter(fd, 0);
				if (listening) {
					cur = s;
					set_filter(fd, s.dest_mac);
					membership(fd, ifindex, s.dest_mac, true);
					rep.stream_id = s.stream_id;
					state = L_WAITING;
					have_seq = false;
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
			if (state == L_RUNNING && !pending && now - last_rx > INTERRUPT_NS) {
				rep.stream_interrupted++;
				if (rep.locked) {
					rep.media_unlocked++;
				}
				rep.locked = 0;
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
		size_t frames = pdu.data_len / (4u * c->channels);
		if (pdu.format != AAF_FORMAT_INT_32BIT || pdu.nsr != AAF_NSR_48KHZ || pdu.channels != c->channels ||
		    pdu.data_len % (4u * c->channels) != 0u || frames == 0u || frames > MAX_FRAMES) {
			rep.unsupported_format++;
			continue;
		}
		rep.frames_rx++;
		if (have_seq && pdu.seq != (uint8_t)(last_seq + 1u)) {
			rep.seq_mismatch++;
		}
		last_seq = pdu.seq;
		have_seq = true;
		last_rx = now;

		// the presentation time: the 64-bit gPTP time nearest the 32-bit stamp
		int64_t t_pres = now + (int32_t)(pdu.avtp_timestamp - (uint32_t)now);
		int64_t margin = t_pres - now;
		if (margin < 0) {
			rep.late_timestamp++;
		} else if (margin > 4 * (int64_t)c->pto_ns) {
			rep.early_timestamp++;
		}
		rep.margin_min_ns = margin < rep.margin_min_ns ? (int32_t)margin : rep.margin_min_ns;
		aaf_samples_to_host(samples, pdu.data, frames * c->channels);
		const int32_t *first = samples;

		if (state == L_WAITING) {
			// place the first frame at its presentation time
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

		// when this PDU's first frame will reach the host, against when it should
		int64_t err = now + (int64_t)snk->delay(snk) * 1000000000 / FS + c->in_flight_ns - t_pres;
		if (err > RESET_NS) {
			size_t drop = (size_t)(err * FS / 1000000000);
			rep.media_resets++;
			if (drop >= frames) {
				continue;
			}
			first += drop * c->channels;
			frames -= drop;
		} else if (err < -RESET_NS) {
			rep.media_resets++;
			write_silence(snk, -err * FS / 1000000000);
		} else {
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
			bool locked = servo.locked;
			if (locked && !rep.locked) {
				rep.media_locked++;
			} else if (!locked && rep.locked) {
				rep.media_unlocked++;
			}
			rep.locked = locked;
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
