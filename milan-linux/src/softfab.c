// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// softfab.c - the soft fabric (see softfab.h) and mbx_hal.h on it.

#define _GNU_SOURCE
#include "softfab.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/filter.h>
#include <linux/if_packet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "mbx_hal.h"

#define RX_PER_PUMP 64u         // frames one pump hands to the model at most
#define WAIT_CAP_MS 1000        // the longest sleep, whatever is due

// Every destination the control plane listens on (IEEE 1722.1-2021 Table B.1,
// IEEE 1722-2016 Table B.7, IEEE 802.1Q-2018 Table 8-1 and 11.2.3.1.3).
static const uint8_t groups[][6] = {
	{0x91, 0xE0, 0xF0, 0x01, 0x00, 0x00},   // ADP, ACMP
	{0x91, 0xE0, 0xF0, 0x01, 0x00, 0x01},   // AECP identify notifications
	{0x91, 0xE0, 0xF0, 0x00, 0xFF, 0x00},   // MAAP
	{0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E},   // MSRP
	{0x01, 0x80, 0xC2, 0x00, 0x00, 0x21},   // MVRP
};

// The prefilter: AVTP (0x22F0) control subtypes (0xF0 and up: ADP, AECP,
// ACMP, MAAP), MSRP (0x22EA) and MVRP (0x88F5). Stream data never wakes the
// control plane; the model's own filter is the authority on the rest.
static struct sock_filter prefilter[] = {
	BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 12),
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x22F0, 2, 0),
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x22EA, 3, 0),
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x88F5, 2, 3),
	BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 14),
	BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0xF0, 0, 1),
	BPF_STMT(BPF_RET | BPF_K, 0xFFFF),
	BPF_STMT(BPF_RET | BPF_K, 0),
};

static struct softfab *bound;

static uint64_t mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint64_t softfab_now_ms(const struct softfab *sf)
{
	return mono_ms() - sf->t0_ms;
}

// ---- mbx_hal.h ---------------------------------------------------------------

// Every frame the model's TX merge sent since the last call, onto the wire.
static void tx_flush(struct softfab *sf)
{
	while (sf->tx_drained != sf->model.tx_sent) {
		const struct mbx_model_tx *f = mbx_model_tx_frame(&sf->model, sf->tx_drained);
		sf->tx_drained++;
		if (f == NULL) {
			sf->stats.tx_lost++;
			continue;
		}
		if (send(sf->pkt_fd, f->bytes, f->len, 0) == (ssize_t)f->len) {
			sf->stats.tx_frames++;
		} else {
			sf->stats.tx_errors++;
		}
	}
}

uint32_t mbx_hal_read32(uint32_t byte_offset)
{
	return mbx_model_read(&bound->model, byte_offset);
}

_Static_assert(MBX_N_PUB_SINKS <= 32u, "a bit per sink of the publication block");

// Each sink's BINDING.BOUND as the publication block holds it after a write,
// against the last seen: every edge, in the write that made it, so a loop pass
// that binds, unbinds and binds again shows each one.
static void bound_edges(struct softfab *sf)
{
	const uint32_t *pub = sf->model.pub[0];
	uint32_t now = 0;
	for (unsigned k = 0; k < MBX_N_PUB_SINKS; ++k) {
		uint32_t binding = pub[(MBX_PUB_SINK_BASE + MBX_PUB_SINK_STRIDE * k + MBX_PUB_SINK_REG_BINDING) / 4u];
		now |= ((binding >> MBX_BINDING_BOUND_LSB) & 1u) << k;
	}
	uint32_t moved = now ^ sf->pub_bound;
	sf->pub_bound = now;
	for (unsigned k = 0; moved != 0u && k < MBX_N_PUB_SINKS; ++k) {
		if ((moved >> k) & 1u) {
			moved &= ~(1u << k);
			if (sf->cfg.bound_changed != NULL) {
				sf->cfg.bound_changed(sf->cfg.bound_ctx, k, (now >> k) & 1u);
			}
		}
	}
}

void mbx_hal_write32(uint32_t byte_offset, uint32_t value)
{
	mbx_model_write(&bound->model, byte_offset, value, 0xFu);
	bound_edges(bound);
	tx_flush(bound);
}

void mbx_hal_wait(void)
{
	softfab_pump(bound, true);
}

// ---- inputs ------------------------------------------------------------------

static void advance(struct softfab *sf)
{
	uint32_t target = (uint32_t)softfab_now_ms(sf);
	uint32_t delta = target - sf->model.now_ms;
	if ((int32_t)delta > 0) {
		mbx_model_advance_ms(&sf->model, delta);
	}
}

static void set_link(struct softfab *sf, bool up)
{
	if (up != sf->link_up) {
		sf->link_up = up;
		mbx_model_set_link(&sf->model, 0, up);
	}
}

static void rx_frames(struct softfab *sf)
{
	uint8_t buf[2048];
	for (unsigned k = 0; k < RX_PER_PUMP; ++k) {
		struct sockaddr_ll from;
		socklen_t flen = sizeof from;
		ssize_t n = recvfrom(sf->pkt_fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &flen);
		if (n < 0) {
			return;
		}
		if (from.sll_pkttype == PACKET_OUTGOING || n < ETH_HLEN) {
			continue;
		}
		sf->stats.rx_frames++;
		if (mbx_model_rx(&sf->model, buf, (size_t)n, 0)) {
			sf->stats.rx_admitted++;
		}
	}
}

static void rx_netlink(struct softfab *sf)
{
	uint8_t buf[8192];
	for (;;) {
		ssize_t n = recv(sf->nl_fd, buf, sizeof buf, 0);
		if (n <= 0) {
			return;
		}
		for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, (unsigned)n); h = NLMSG_NEXT(h, n)) {
			if (h->nlmsg_type != RTM_NEWLINK && h->nlmsg_type != RTM_DELLINK) {
				continue;
			}
			const struct ifinfomsg *ifi = NLMSG_DATA(h);
			if (ifi->ifi_index != sf->ifindex) {
				continue;
			}
			set_link(sf, h->nlmsg_type == RTM_NEWLINK && (ifi->ifi_flags & IFF_UP) &&
					     (ifi->ifi_flags & IFF_RUNNING));
		}
	}
}

// flexptpd rewrites its block every 50 ms and counts each rewrite in `updates`.
// A block whose count has not moved for this long, or that could not be read
// for this long (absent, another layout, its writer stuck mid-copy with `seq`
// odd), belongs to a flexptpd that is gone. A new one makes a new block under
// the same name, so the mapping is dropped and made again at the next poll.
#define GPTP_STALE_MS 2000u

// Freshness. The status is live (gptp_seen) only while `updates` is seen to
// move: the first read of a block, a block mapped again, or a block that
// replaced the old one under its name, is not yet evidence of a live
// publisher, since a gone flexptpd leaves its last block behind. Only a change
// of `updates` against the last value read makes the status live, and only
// that keeps it live. Expiry clears what the far side reports from the status
// (CLOCK_DOMAIN's lock, asCapable, the link delay, the path): one UNLOCKED,
// and no LOCKED until flexptpd is seen to run again. The grandmaster the
// control plane announces is left as it was, so a publisher's stall or
// restart is not a grandmaster change.
static void poll_gptp(struct softfab *sf)
{
	if (!sf->gptp_open) {
		return;
	}

	uint64_t now = softfab_now_ms(sf);
	if (now < sf->gptp_next_ms) {
		return;
	}
	sf->gptp_next_ms = now + sf->cfg.gptp_poll_ms;

	// flexptpd not running yet is not an error: the next poll looks again
	struct gptp_status st;
	bool read = gptp_shm_read(&sf->gptp, &st);
	bool moved = false;
	if (read) {
		sf->stats.gptp_reads++;
		moved = sf->gptp_counted && st.updates != sf->gptp_updates;
		if (!sf->gptp_counted || moved) {
			sf->gptp_counted = true;
			sf->gptp_updates = st.updates;
			sf->gptp_fresh_ms = now;
		}
	}
	if (!moved) {
		if (now - sf->gptp_fresh_ms > GPTP_STALE_MS) {
			if (sf->gptp_seen) {
				sf->stats.gptp_expiries++;
			}
			sf->gptp_seen = false;
			sf->as_capable = false;
			// map it again at the next poll, in case a new flexptpd made a new block;
			// the count read there is a baseline, not a change
			gptp_shm_close(&sf->gptp);
			sf->gptp_counted = false;
			sf->gptp_fresh_ms = now;
		} else if (read && sf->gptp_seen) {
			sf->gptp_last = st;     // unchanged since the last update, and still within its life
		}
		return;
	}

	sf->gptp_last = st;
	sf->gptp_seen = true;
	sf->as_capable = st.as_capable;

	uint64_t gm = gptp_status_grandmaster(&st);

	if (!sf->gm_known || gm != sf->gm_id || st.domain != sf->gm_domain) {
		sf->gm_known = true;
		sf->gm_id = gm;
		sf->gm_domain = st.domain;
		sf->stats.gm_changes++;
		mbx_model_gm_change(&sf->model, 0, gm, st.domain);
	}
}

// Milliseconds until the model next needs time to pass: its earliest armed
// timer, its next TICK, the next gPTP poll; 0 while it has work posted.
static int wait_ms(struct softfab *sf)
{
	const struct mbx_model *m = &sf->model;
	if (mbx_model_irq(m)) {
		return 0;
	}
	int64_t wait = WAIT_CAP_MS;
	for (unsigned s = 0; s < MBX_N_TIMERS; ++s) {
		if (m->timers[s].armed) {
			int32_t d = (int32_t)(m->timers[s].deadline_ms - m->now_ms);
			wait = d < wait ? d : wait;
		}
	}
	if (m->tick_ctl != 0u) {
		int64_t d = (int64_t)MBX_TICK_MS - m->tick_div;
		wait = d < wait ? d : wait;
	}
	if (sf->gptp_open) {
		int64_t d = (int64_t)sf->gptp_next_ms - (int64_t)softfab_now_ms(sf);
		wait = d < wait ? d : wait;
	}
	return wait < 0 ? 0 : (int)wait;
}

void softfab_pump(struct softfab *sf, bool block)
{
	struct epoll_event ev[4];
	int n = epoll_wait(sf->ep_fd, ev, 4, block ? wait_ms(sf) : 0);
	advance(sf);
	for (int k = 0; k < n; ++k) {
		if (ev[k].data.fd == sf->pkt_fd) {
			rx_frames(sf);
		} else if (ev[k].data.fd == sf->nl_fd) {
			rx_netlink(sf);
		}
	}
	poll_gptp(sf);
	tx_flush(sf);
}

// ---- setup -------------------------------------------------------------------

static int open_packet(struct softfab *sf)
{
	sf->pkt_fd = socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, htons(ETH_P_ALL));
	if (sf->pkt_fd < 0) {
		return -errno;
	}
	struct sock_fprog prog = {sizeof prefilter / sizeof prefilter[0], prefilter};
	int one = 1;
	if (setsockopt(sf->pkt_fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof prog) != 0) {
		return -errno;
	}
	// our own transmissions would come back as PACKET_OUTGOING; the kernel
	// can drop them before they are queued (Linux 4.20)
	(void)setsockopt(sf->pkt_fd, SOL_PACKET, PACKET_IGNORE_OUTGOING, &one, sizeof one);
	struct sockaddr_ll sll = {
		.sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL), .sll_ifindex = sf->ifindex,
	};
	if (bind(sf->pkt_fd, (struct sockaddr *)&sll, sizeof sll) != 0) {
		return -errno;
	}
	for (unsigned g = 0; g < sizeof groups / sizeof groups[0]; ++g) {
		struct packet_mreq mr = {.mr_ifindex = sf->ifindex, .mr_type = PACKET_MR_MULTICAST, .mr_alen = 6};
		memcpy(mr.mr_address, groups[g], 6);
		if (setsockopt(sf->pkt_fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof mr) != 0) {
			return -errno;
		}
	}
	// what arrived between socket() and the filter, from any interface
	uint8_t junk[2048];
	while (recv(sf->pkt_fd, junk, sizeof junk, 0) > 0) {
	}
	return 0;
}

static int open_netlink(struct softfab *sf)
{
	sf->nl_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (sf->nl_fd < 0) {
		return -errno;
	}
	struct sockaddr_nl snl = {.nl_family = AF_NETLINK, .nl_groups = RTMGRP_LINK};
	return bind(sf->nl_fd, (struct sockaddr *)&snl, sizeof snl) == 0 ? 0 : -errno;
}

static int if_info(struct softfab *sf, bool *running)
{
	struct ifreq ifr;
	memset(&ifr, 0, sizeof ifr);
	if (strlen(sf->cfg.ifname) >= sizeof ifr.ifr_name) {
		return -ENAMETOOLONG;
	}
	strcpy(ifr.ifr_name, sf->cfg.ifname);
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -errno;
	}
	int err = 0;
	if (ioctl(fd, SIOCGIFINDEX, &ifr) != 0) {
		err = -errno;
	} else {
		sf->ifindex = ifr.ifr_ifindex;
		if (ioctl(fd, SIOCGIFHWADDR, &ifr) != 0) {
			err = -errno;
		} else {
			memcpy(sf->mac, ifr.ifr_hwaddr.sa_data, 6);
			if (ioctl(fd, SIOCGIFFLAGS, &ifr) != 0) {
				err = -errno;
			} else {
				*running = (ifr.ifr_flags & IFF_UP) && (ifr.ifr_flags & IFF_RUNNING);
			}
		}
	}
	close(fd);
	return err;
}

static int watch(struct softfab *sf, int fd)
{
	struct epoll_event ev = {.events = EPOLLIN, .data.fd = fd};
	return epoll_ctl(sf->ep_fd, EPOLL_CTL_ADD, fd, &ev) == 0 ? 0 : -errno;
}

int softfab_open(struct softfab *sf, const struct softfab_config *cfg)
{
	memset(sf, 0, sizeof *sf);
	sf->cfg = *cfg;
	sf->pkt_fd = sf->nl_fd = sf->ep_fd = -1;
	mbx_model_reset(&sf->model);
	sf->t0_ms = mono_ms();
	bound = sf;

	bool running = false;
	int err = if_info(sf, &running);
	const char *what = "interface";
	if (err == 0) {
		what = "netlink";
		err = open_netlink(sf);
	}
	if (err == 0) {
		what = "AF_PACKET";
		err = open_packet(sf);
	}
	if (err == 0) {
		what = "epoll";
		sf->ep_fd = epoll_create1(EPOLL_CLOEXEC);
		err = sf->ep_fd < 0 ? -errno : 0;
	}
	if (err == 0) {
		err = watch(sf, sf->pkt_fd);
	}
	if (err == 0) {
		err = watch(sf, sf->nl_fd);
	}
	if (err == 0 && cfg->gptp_shm != NULL) {
		what = "flexptpd status block name";
		err = gptp_shm_open(&sf->gptp, cfg->gptp_shm);
		sf->gptp_open = err == 0;
	}
	if (err != 0) {
		fprintf(stderr, "softfab: %s on %s: %s\n", what, cfg->ifname, strerror(-err));
		softfab_close(sf);
		return err;
	}
	set_link(sf, running);
	return 0;
}

void softfab_close(struct softfab *sf)
{
	if (sf->gptp_open) {
		gptp_shm_close(&sf->gptp);
		sf->gptp_open = false;
	}
	if (sf->ep_fd >= 0) {
		close(sf->ep_fd);
	}
	if (sf->pkt_fd >= 0) {
		close(sf->pkt_fd);
	}
	if (sf->nl_fd >= 0) {
		close(sf->nl_fd);
	}
	sf->ep_fd = sf->pkt_fd = sf->nl_fd = -1;
}
