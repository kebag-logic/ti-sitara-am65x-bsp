// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// crf_rx.c - see crf_rx.h.

#define _GNU_SOURCE
#include "crf_rx.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/filter.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <sched.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "crf.h"

#define CHECK_NS 10000000               // the datapath block is read every 10 ms
#define REPORT_NS 100000000             // the report is published every 100 ms

enum cstate { C_NONE, C_WAITING, C_RUNNING };

static void live_update(struct crf_rx *x, const struct milan_media_crf *r)
{
	shmblk_begin(&x->live);
	x->live.r = *r;
	shmblk_end(&x->live);
}

void crf_rx_report(const struct crf_rx *x, struct milan_media_crf *out)
{
	struct crf_rx_live copy;
	if (shmblk_snapshot(&x->live, &copy, sizeof copy)) {
		*out = copy.r;
	}
}

static bool sink_of(const struct crf_rx *x, struct milan_dp_sink *k)
{
	struct milan_dp dp;
	if (!milan_dp_snapshot(x->cfg.dp, &dp) || x->cfg.sink >= dp.n_sinks) {
		return false;
	}
	*k = dp.sinks[x->cfg.sink];
	return true;
}

// The stream's frames only: its destination, AVTP, subtype CRF, tagged or not.
static int set_filter(int fd, uint64_t mac)
{
	uint32_t hi = (uint32_t)(mac >> 16);
	uint32_t lo = (uint32_t)(mac & 0xFFFFu);
	struct sock_filter f[] = {
		/* 0 */ BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
		/* 1 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, hi, 0, 12),
		/* 2 */ BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 4),
		/* 3 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, lo, 0, 10),
		/* 4 */ BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 12),
		/* 5 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x8100, 0, 4),
		/* 6 */ BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 16),
		/* 7 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x22F0, 0, 6),
		/* 8 */ BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 18),
		/* 9 */ BPF_JUMP(BPF_JMP | BPF_JA, 2, 0, 0),
		/* 10 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x22F0, 0, 3),
		/* 11 */ BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 14),
		/* 12 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, CRF_SUBTYPE, 0, 1),
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

static void set_locked(struct milan_media_crf *r, bool locked)
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

// The media clock the stream carries, as its timestamps lay it out.
struct grid {
	bool have;                      // a last stamp to step from
	uint8_t seq;                    // its PDU's sequence_num
	uint64_t ts;                    // the last timestamp of that PDU
	unsigned steps;                 // steps on the grid in a row
	uint64_t run_ts;                // the first stamp of the current run, and
	uint64_t run_events;            // the events since it, at the nominal step
};

static void *crf_rx_main(void *arg)
{
	struct crf_rx *x = arg;
	const struct crf_rx_cfg *c = &x->cfg;
	struct milan_media_crf rep;
	memset(&rep, 0, sizeof rep);
	rep.margin_min_ns = INT32_MAX;
	rep.milan.valid = MILAN_SI_VALID;       // this thread counts all ten (media.h)
	struct milan_obs obs;
	milan_counters_zero(&rep.milan, &obs, 0, milan_mono_ns());
	live_update(x, &rep);

	int ifindex = (int)if_nametoindex(c->ifname);
	int fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
	struct sockaddr_ll sll = {.sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL), .sll_ifindex = ifindex};
	struct timeval tv = {0, 5000};
	if (fd < 0 || ifindex == 0 || set_filter(fd, 0) != 0 || bind(fd, (struct sockaddr *)&sll, sizeof sll) != 0 ||
	    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0) {
		// no receive path: nothing counts the input, so none of its counters is valid
		rep.milan.valid = 0;
		live_update(x, &rep);
		if (fd >= 0) {
			close(fd);
		}
		return NULL;
	}
	int one = 1;
	(void)setsockopt(fd, SOL_PACKET, PACKET_IGNORE_OUTGOING, &one, sizeof one);

	// the nominal step between two timestamps of the Milan format, in ns:
	// timestamp_interval events at base_frequency (pull 0)
	const uint64_t step_ns = (uint64_t)CRF_MILAN_INTERVAL * 1000000000u / CRF_MILAN_BASE_FREQUENCY;

	enum cstate state = C_NONE;
	uint64_t cur_stream = 0, cur_mac = 0;
	bool have_seq = false, have_mr = false, last_mr = false;
	uint8_t last_seq = 0;
	struct grid g = {0};
	int64_t last_rx = 0, next_check = 0, next_report = 0;
	uint8_t frame[2048];

	while (!atomic_load(&x->stop)) {
		bool publish = milan_obs_close(&obs, &rep.milan, milan_mono_ns());
		int64_t now = gptp_now(c->clk);
		if (now >= next_check) {
			next_check = now + CHECK_NS;
			struct milan_dp_sink k;
			bool read = sink_of(x, &k);
			bool listening = read && k.listening;
			bool rebound = read && k.binds != rep.milan.binds;
			if (rebound || !listening || k.stream_id != cur_stream || k.dest_mac != cur_mac) {
				if (state != C_NONE) {
					membership(fd, ifindex, cur_mac, false);
					set_locked(&rep, false);
					rep.active = 0;
				}
				if (rebound) {
					milan_counters_zero(&rep.milan, &obs, 0, milan_mono_ns());
					rep.milan.binds = k.binds;
				}
				state = C_NONE;
				cur_stream = cur_mac = 0;
				set_filter(fd, 0);
				if (listening) {
					cur_stream = k.stream_id;
					cur_mac = k.dest_mac;
					set_filter(fd, k.dest_mac);
					membership(fd, ifindex, k.dest_mac, true);
					rep.stream_id = k.stream_id;
					state = C_WAITING;
					have_seq = have_mr = false;
					g.have = false;
				}
				publish = true;
			}
		}
		bool report = now >= next_report;
		if (publish || report) {
			live_update(x, &rep);
		}
		if (report) {
			// the interval figures start over for the next report
			next_report = now + REPORT_NS;
			rep.step_err_max_ns = 0;
			rep.margin_min_ns = INT32_MAX;
		}

		ssize_t n = recv(fd, frame, sizeof frame, 0);
		now = gptp_now(c->clk);
		struct crf_pdu pdu;
		bool ours = n > 0 && state != C_NONE && crf_parse(frame, (size_t)n, &pdu) && pdu.stream_id == cur_stream;
		if (!ours || !crf_is_milan(&pdu)) {
			// no Milan PDU of the stream for interrupt_ns, PDUs of another format
			// arriving or none: a timeout can race PDUs that came while this thread
			// waited to run, and with any waiting the stream was not interrupted
			uint8_t peek;
			bool pending = n <= 0 && recv(fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT) > 0;
			if (state == C_RUNNING && !pending && now - last_rx > c->interrupt_ns) {
				rep.stream_interrupted++;
				rep.milan.value[MILAN_SI_STREAM_INTERRUPTED]++;
				set_locked(&rep, false);
				rep.active = 0;
				state = C_WAITING;
				have_seq = false;
				g.have = false;
				live_update(x, &rep);
			}
			if (!ours) {
				continue;
			}
		}
		// received, whatever is in it; then its header, as the AAF listener reads one
		rep.pdus++;
		milan_obs_note(&obs, MILAN_SI_FRAMES_RX);
		bool skipped = have_seq && pdu.seq != (uint8_t)(last_seq + 1u);
		if (skipped) {
			rep.seq_mismatch++;
			milan_obs_note(&obs, MILAN_SI_SEQ_NUM_MISMATCH);
		}
		last_seq = pdu.seq;
		have_seq = true;
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
		if (!crf_is_milan(&pdu)) {
			rep.unsupported_format++;
			milan_obs_note(&obs, MILAN_SI_UNSUPPORTED_FORMAT);
			continue;
		}
		// each timestamp, a presentation time (crf_rx.h)
		int64_t margin = (int64_t)(pdu.timestamps[0] - (uint64_t)now);
		if (margin < 0) {
			rep.late_timestamp++;
			milan_obs_note(&obs, MILAN_SI_LATE_TIMESTAMP);
		} else if (margin > c->early_ns) {
			// too far in the future to process: counted, and not laid on the grid
			rep.early_timestamp++;
			milan_obs_note(&obs, MILAN_SI_EARLY_TIMESTAMP);
			continue;
		}
		int32_t m32 = margin < INT32_MIN ? INT32_MIN : margin > INT32_MAX ? INT32_MAX : (int32_t)margin;
		rep.margin_min_ns = m32 < rep.margin_min_ns ? m32 : rep.margin_min_ns;
		last_rx = now;
		state = C_RUNNING;
		rep.active = 1;

		// the grid: a restart or an uncertain stamp starts it over
		if (restart) {
			set_locked(&rep, false);
			g.have = false;
		}
		uint64_t ts = pdu.timestamps[0];
		if (pdu.tu || !g.have) {
			g = (struct grid){.have = !pdu.tu, .seq = pdu.seq, .ts = ts, .run_ts = ts};
			continue;
		}
		uint8_t dseq = (uint8_t)(pdu.seq - g.seq);
		int64_t err = (int64_t)(ts - g.ts) - (int64_t)(dseq * step_ns);
		int64_t abs_err = err < 0 ? -err : err;
		rep.step_err_max_ns = abs_err > rep.step_err_max_ns ? (int32_t)(abs_err > INT32_MAX ? INT32_MAX : abs_err)
								 : rep.step_err_max_ns;
		if (dseq == 0u || abs_err > CRF_RX_STEP_TOL_NS) {
			// off the grid: the clock this stream carries is not the one it was
			rep.grid_breaks++;
			set_locked(&rep, false);
			g = (struct grid){.have = true, .seq = pdu.seq, .ts = ts, .run_ts = ts};
			continue;
		}
		g.seq = pdu.seq;
		g.ts = ts;
		g.steps++;
		g.run_events += dseq;
		if (g.steps >= CRF_RX_LOCK_STEPS && !rep.locked) {
			set_locked(&rep, true);
		}
		if (g.run_events > 0u) {
			double nominal = (double)g.run_events * (double)step_ns;
			rep.rate_ppb = (int64_t)(((double)(ts - g.run_ts) / nominal - 1.0) * 1e9);
		}
	}
	close(fd);
	return NULL;
}

int crf_rx_start(struct crf_rx *x, const struct crf_rx_cfg *cfg)
{
	memset(x, 0, sizeof *x);
	x->cfg = *cfg;
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
	int rc = pthread_create(&x->thread, &attr, crf_rx_main, x);
	pthread_attr_destroy(&attr);
	return rc;
}

void crf_rx_stop(struct crf_rx *x)
{
	atomic_store(&x->stop, true);
	pthread_join(x->thread, NULL);
}
