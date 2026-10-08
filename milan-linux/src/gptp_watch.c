// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// gptp_watch.c - milan-gptp-watch, a judge of gPTP on the AVB interface that
// does not depend on the gPTP daemon.
//
// It reads the grandmaster's Sync and Follow_Up as they arrive, with the
// board's own hardware receive timestamp of each Sync, and computes how far
// the PHC is from the grandmaster:
//
//     offset = t_rx(Sync) - (preciseOriginTimestamp + correction + link delay)
//
// whichever daemon steers the PHC (ptp4l, or another stack). The daemon has
// hardware receive timestamping on, since it runs gPTP; this tool only asks
// for the timestamps the interface already produces. The link delay is given
// on the command line: another process's Pdelay_Req transmit time cannot be
// seen from here.
//
// The grandmaster comes from the Announce messages. The verdict asks for
// enough samples, every one with a hardware timestamp, |offset| within the
// limit for 99.9 % of them, and no change of grandmaster.

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <linux/errqueue.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define ETH_P_1588 0x88F7
#define ETH_HLEN 14

#define MSG_SYNC 0x0
#define MSG_FOLLOW_UP 0x8
#define MSG_ANNOUNCE 0xB

#define PTP_HDR_LEN 34
#define MAX_SAMPLES (1u << 18)          // 9 h of Syncs at 8 per second

static const uint8_t GPTP_MC[6] = {0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E};

static volatile sig_atomic_t stop;

struct ptp_hdr {
	unsigned type;
	unsigned sdo;                   // majorSdoId: 1 for gPTP
	int64_t correction_ns;          // correctionField, sub-ns dropped
	uint8_t port[10];               // sourcePortIdentity
	uint16_t seq;
	int8_t log_interval;
};

struct pending_sync {
	bool valid;
	uint8_t port[10];
	uint16_t seq;
	int64_t rx_ns;                  // 0: the Sync came without a hardware timestamp
	int64_t correction_ns;
};

struct window {
	unsigned n;
	int64_t min;
	int64_t max;
	int64_t sum;
};

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t be48(const uint8_t *p)
{
	return (uint64_t)be16(p) << 32 | be32(p + 2);
}

static uint64_t be64(const uint8_t *p)
{
	return (uint64_t)be32(p) << 32 | be32(p + 4);
}

static int64_t mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void fmt_clock_id(char *out, size_t len, const uint8_t *id)
{
	snprintf(out, len, "%02x%02x%02x.%02x%02x.%02x%02x%02x",
		 id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
}

static bool parse_hdr(const uint8_t *p, size_t n, struct ptp_hdr *h)
{
	if (n < PTP_HDR_LEN) {
		return false;
	}

	h->type = p[0] & 0x0F;
	h->sdo = p[0] >> 4;

	// scaled ns (2^-16): divide rather than shift, the field is signed
	h->correction_ns = (int64_t)be64(p + 8) / 65536;

	memcpy(h->port, p + 20, sizeof h->port);
	h->seq = be16(p + 30);
	h->log_interval = (int8_t)p[33];

	return true;
}

static int open_socket(const char *ifname)
{
	int fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_1588));
	if (fd < 0) {
		perror("socket");
		return -1;
	}

	unsigned ifindex = if_nametoindex(ifname);
	if (ifindex == 0) {
		fprintf(stderr, "gptp-watch: no interface %s\n", ifname);
		close(fd);
		return -1;
	}

	struct sockaddr_ll sll = {
		.sll_family = AF_PACKET,
		.sll_protocol = htons(ETH_P_1588),
		.sll_ifindex = (int)ifindex,
	};
	if (bind(fd, (struct sockaddr *)&sll, sizeof sll) != 0) {
		perror("bind");
		close(fd);
		return -1;
	}

	// the daemon has joined the group already; joining again costs nothing
	struct packet_mreq mreq = {
		.mr_ifindex = (int)ifindex,
		.mr_type = PACKET_MR_MULTICAST,
		.mr_alen = 6,
	};
	memcpy(mreq.mr_address, GPTP_MC, sizeof GPTP_MC);
	setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof mreq);

	// report the hardware timestamps; the interface makes them for the daemon
	int flags = SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE;
	if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof flags) != 0) {
		perror("SO_TIMESTAMPING");
		close(fd);
		return -1;
	}

	return fd;
}

// One frame, its hardware receive time (0 if none), and whether the board
// sent it itself (an outgoing frame looped back to the packet socket).
static ssize_t receive(int fd, uint8_t *buf, size_t len, int64_t *rx_ns, bool *outgoing)
{
	char ctrl[256];
	struct sockaddr_ll from;
	struct iovec iov = {buf, len};
	struct msghdr msg = {
		.msg_name = &from,
		.msg_namelen = sizeof from,
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = ctrl,
		.msg_controllen = sizeof ctrl,
	};

	ssize_t n = recvmsg(fd, &msg, MSG_DONTWAIT);
	if (n < 0) {
		return n;
	}

	*outgoing = from.sll_pkttype == PACKET_OUTGOING;

	*rx_ns = 0;
	for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c)) {
		if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SO_TIMESTAMPING) {
			continue;
		}

		struct scm_timestamping ts;
		memcpy(&ts, CMSG_DATA(c), sizeof ts);

		// [2] is the raw hardware time: the PHC's
		*rx_ns = (int64_t)ts.ts[2].tv_sec * 1000000000 + ts.ts[2].tv_nsec;
	}

	return n;
}

static int cmp_i64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a;
	int64_t y = *(const int64_t *)b;

	return (x > y) - (x < y);
}

static int64_t percentile(const int64_t *sorted, size_t n, double p)
{
	if (n == 0) {
		return 0;
	}

	size_t i = (size_t)(p * (double)(n - 1) + 0.5);
	return sorted[i];
}

static void usage(FILE *to)
{
	fprintf(to,
		"usage: milan-gptp-watch -i IF [-d SECONDS] [-w SECONDS] [-l NS] [-t NS] [-g GM] [-q]\n"
		"  -i IF       the gPTP interface\n"
		"  -d SECONDS  how long to watch (60)\n"
		"  -w SECONDS  warm-up left out of the verdict (0)\n"
		"  -l NS       the link delay to the neighbour (0); ptp4l measured 203 on the bench\n"
		"  -t NS       the |offset| limit (100)\n"
		"  -g GM       the grandmaster expected, as 3cc0c6.fffe.fe0210\n"
		"  -q          no line per second, the summary only\n"
		"Exit 0 = PASS, 1 = FAIL, 2 = usage or setup error.\n");
}

int main(int argc, char **argv)
{
	const char *ifname = NULL;
	const char *want_gm = NULL;
	double duration_s = 60;
	double warmup_s = 0;
	int64_t link_ns = 0;
	int64_t limit_ns = 100;
	bool quiet = false;

	int opt;
	while ((opt = getopt(argc, argv, "i:d:w:l:t:g:qh")) != -1) {
		switch (opt) {
		case 'i':
			ifname = optarg;
			break;
		case 'd':
			duration_s = atof(optarg);
			break;
		case 'w':
			warmup_s = atof(optarg);
			break;
		case 'l':
			link_ns = atoll(optarg);
			break;
		case 't':
			limit_ns = atoll(optarg);
			break;
		case 'g':
			want_gm = optarg;
			break;
		case 'q':
			quiet = true;
			break;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}

	if (ifname == NULL || duration_s <= 0 || warmup_s < 0 || warmup_s >= duration_s) {
		usage(stderr);
		return 2;
	}

	int fd = open_socket(ifname);
	if (fd < 0) {
		return 2;
	}

	int64_t *samples = calloc(MAX_SAMPLES, sizeof *samples);
	if (samples == NULL) {
		perror("calloc");
		return 2;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	setvbuf(stdout, NULL, _IOLBF, 0);

	printf("watching gPTP on %s for %.0f s (warm-up %.0f s), link delay %" PRId64 " ns, limit %" PRId64 " ns\n",
	       ifname, duration_s, warmup_s, link_ns, limit_ns);

	struct pending_sync pending = {0};
	struct window win = {0};

	char gm[32] = "";
	unsigned gm_changes = 0;
	unsigned gm_priority1 = 0;
	unsigned steps_removed = 0;

	size_t n_samples = 0;
	unsigned no_hwts = 0;
	unsigned missed = 0;
	unsigned sdo_wrong = 0;
	int8_t log_sync = 127;
	bool have_seq = false;
	uint16_t last_seq = 0;

	int64_t t_start = mono_ns();
	int64_t t_warm = t_start + (int64_t)(warmup_s * 1e9);
	int64_t t_end = t_start + (int64_t)(duration_s * 1e9);
	int64_t t_report = t_start + 1000000000;

	while (!stop) {
		int64_t now = mono_ns();
		if (now >= t_end) {
			break;
		}

		if (now >= t_report) {
			if (!quiet) {
				if (win.n > 0) {
					printf("t=%3" PRId64 "s gm %s syncs %u offset min %" PRId64 " avg %" PRId64 " max %" PRId64 " ns\n",
					       (now - t_start) / 1000000000, gm[0] ? gm : "-", win.n,
					       win.min, win.sum / win.n, win.max);
				} else {
					printf("t=%3" PRId64 "s gm %s no Sync with a Follow_Up\n",
					       (now - t_start) / 1000000000, gm[0] ? gm : "-");
				}
			}

			win = (struct window){0};
			t_report += 1000000000;
		}

		struct pollfd pfd = {fd, POLLIN, 0};
		if (poll(&pfd, 1, 100) <= 0) {
			continue;
		}

		uint8_t buf[1600];
		int64_t rx_ns;
		bool outgoing;

		ssize_t n = receive(fd, buf, sizeof buf, &rx_ns, &outgoing);
		if (n < ETH_HLEN + PTP_HDR_LEN || outgoing) {
			continue;
		}

		const uint8_t *p = buf + ETH_HLEN;
		size_t len = (size_t)n - ETH_HLEN;

		struct ptp_hdr h;
		if (!parse_hdr(p, len, &h)) {
			continue;
		}

		if (h.sdo != 1) {
			sdo_wrong++;
			continue;
		}

		bool counting = mono_ns() >= t_warm;

		if (h.type == MSG_ANNOUNCE && len >= 64) {
			char id[32];
			fmt_clock_id(id, sizeof id, p + 53);

			if (gm[0] != '\0' && strcmp(id, gm) != 0) {
				printf("grandmaster change: %s -> %s\n", gm, id);
				if (counting) {
					gm_changes++;
				}
			}

			snprintf(gm, sizeof gm, "%s", id);
			gm_priority1 = p[47];
			steps_removed = be16(p + 61);

		} else if (h.type == MSG_SYNC) {
			// a Sync lost on the way shows as a gap in its sequence
			if (have_seq && counting) {
				uint16_t gap = (uint16_t)(h.seq - last_seq);
				if (gap > 1 && gap < 1000) {
					missed += gap - 1u;
				}
			}
			have_seq = true;
			last_seq = h.seq;
			log_sync = h.log_interval;

			pending = (struct pending_sync){
				.valid = true,
				.seq = h.seq,
				.rx_ns = rx_ns,
				.correction_ns = h.correction_ns,
			};
			memcpy(pending.port, h.port, sizeof pending.port);

		} else if (h.type == MSG_FOLLOW_UP && len >= PTP_HDR_LEN + 10) {
			bool match = pending.valid
				&& pending.seq == h.seq
				&& memcmp(pending.port, h.port, sizeof h.port) == 0;
			if (!match) {
				continue;
			}
			pending.valid = false;

			if (pending.rx_ns == 0) {
				if (counting) {
					no_hwts++;
				}
				continue;
			}

			int64_t origin = (int64_t)be48(p + 34) * 1000000000 + be32(p + 40);
			int64_t master = origin + pending.correction_ns + h.correction_ns + link_ns;
			int64_t offset = pending.rx_ns - master;

			if (win.n == 0 || offset < win.min) {
				win.min = offset;
			}
			if (win.n == 0 || offset > win.max) {
				win.max = offset;
			}
			win.sum += offset;
			win.n++;

			if (counting && n_samples < MAX_SAMPLES) {
				samples[n_samples] = offset;
				n_samples++;
			}
		}
	}

	close(fd);

	// the verdict, over the samples after the warm-up
	if (log_sync == 127) {
		log_sync = -3;                  // no Sync seen: judge against gPTP's 8 per second
	}

	// Syncs per second: 2^-logMessageInterval
	double rate = log_sync >= 0 ? 1.0 / (double)(1 << log_sync) : (double)(1 << -log_sync);

	double judged_s = (double)(mono_ns() - t_warm) / 1e9;
	double expected = judged_s * rate;

	size_t within = 0;
	int64_t worst = 0;                      // the sample of the largest |offset|, signed
	int64_t worst_abs = 0;
	int64_t sum = 0;
	for (size_t i = 0; i < n_samples; ++i) {
		int64_t a = samples[i] < 0 ? -samples[i] : samples[i];

		if (a <= limit_ns) {
			within++;
		}

		if (a > worst_abs) {
			worst_abs = a;
			worst = samples[i];
		}

		sum += samples[i];
	}

	int64_t *sorted = calloc(n_samples ? n_samples : 1, sizeof *sorted);
	for (size_t i = 0; i < n_samples; ++i) {
		sorted[i] = samples[i] < 0 ? -samples[i] : samples[i];
	}
	qsort(sorted, n_samples, sizeof *sorted, cmp_i64);

	double share = n_samples ? 100.0 * (double)within / (double)n_samples : 0;

	printf("grandmaster %s (priority1 %u, steps removed %u)%s%s\n",
	       gm[0] ? gm : "none seen", gm_priority1, steps_removed,
	       want_gm ? ", expected " : "", want_gm ? want_gm : "");
	printf("samples %zu of about %.0f expected (Sync log interval %d), missed Syncs %u, without hw timestamp %u\n",
	       n_samples, expected, log_sync, missed, no_hwts);
	printf("offset mean %" PRId64 " ns, |offset| p50 %" PRId64 " p99 %" PRId64 " p99.9 %" PRId64
	       " max %" PRId64 " ns (worst signed %" PRId64 ")\n",
	       n_samples ? sum / (int64_t)n_samples : 0,
	       percentile(sorted, n_samples, 0.50), percentile(sorted, n_samples, 0.99),
	       percentile(sorted, n_samples, 0.999), n_samples ? sorted[n_samples - 1] : 0, worst);
	printf("|offset| <= %" PRId64 " ns for %.3f %% (want >= 99.9 %%), grandmaster changes %u, other SDO frames %u\n",
	       limit_ns, share, gm_changes, sdo_wrong);

	bool ok = n_samples >= (size_t)(0.9 * expected)
		&& no_hwts == 0
		&& share >= 99.9
		&& gm_changes == 0
		&& (want_gm == NULL || strcmp(gm, want_gm) == 0);

	printf("RESULT: %s\n", ok ? "PASS" : "FAIL");

	free(sorted);
	free(samples);

	return ok ? 0 : 1;
}
