// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// softfab.h - the soft fabric: what the FPGA fabric does for the Mark II
// control plane of the RISC-V end station, done in software on Linux, so the
// same firmware runs unchanged on the PocketBeagle 2.
//
// The firmware reaches its fabric only through mbx_hal.h (read32, write32,
// wait). Here those three calls land on milan-fpga's own C model of the
// fabric side of the mailbox contract (sw/firmware/ctrl/host/mbx_model.c),
// which the mailbox suite grades against the RTL, and this file feeds the
// model from the system:
//
//   fabric duty                  on Linux
//   ---------------------------  ---------------------------------------------
//   ingress filter, RX rings     AF_PACKET on the interface, a BPF prefilter
//                                (AVTP control subtypes, MSRP, MVRP), then the
//                                model's own full-tuple filter
//   TX merge                     each frame the model sends goes out on the
//                                socket inside the write that committed it
//   timer bank, TICK             the model's millisecond clock, advanced to
//                                CLOCK_MONOTONIC on every wake
//   link level                   rtnetlink link messages (IFF_RUNNING)
//   gPTP plane                   ptp4l's read-only management socket,
//                                TIME_STATUS_NP polled for the grandmaster
//   interrupt and sleep          epoll on the socket, netlink and ptp4l, with
//                                a timeout at the next model deadline
//
// One interface (MBX_N_IF is 1 in the shipping contract). One thread.

#ifndef MILAN_SOFTFAB_H
#define MILAN_SOFTFAB_H

#include <stdbool.h>
#include <stdint.h>

#include "mbx_model.h"
#include "ptp_mgmt.h"

struct softfab_config {
	const char *ifname;
	const char *ptp_server;         // ptp4l's read-only socket; NULL = no gPTP plane
	const char *ptp_local;          // our socket for the replies
	uint8_t ptp_transport_specific; // 1 for gPTP
	unsigned ptp_poll_ms;           // TIME_STATUS_NP period
};

struct softfab_stats {
	uint64_t rx_frames;             // frames handed to the model
	uint64_t rx_admitted;           // ... that it committed to a receive ring
	uint64_t tx_frames;             // frames the model sent and we transmitted
	uint64_t tx_errors;             // sendto() failures
	uint64_t tx_lost;               // frames the capture lost before we sent them
	uint64_t gm_changes;
	uint64_t ptp_replies;
};

struct softfab {
	struct mbx_model model;
	struct softfab_config cfg;
	int pkt_fd;
	int nl_fd;
	int ep_fd;
	int ifindex;
	uint8_t mac[6];
	uint64_t t0_ms;                 // CLOCK_MONOTONIC at model time 0
	uint32_t tx_drained;            // the next model TX frame to transmit
	bool link_up;
	bool gm_known;
	uint64_t gm_id;
	uint8_t gm_domain;
	bool as_capable;
	struct ptp_mgmt ptp;
	bool ptp_open;
	uint64_t ptp_next_ms;
	struct softfab_stats stats;
};

// Open the interface, the sockets and the model; makes this the fabric that
// mbx_hal.h reaches. 0, or -errno with a message on stderr.
int softfab_open(struct softfab *sf, const struct softfab_config *cfg);
void softfab_close(struct softfab *sf);

// Bring the model up to date with the system: advance its clock to now, give
// it every waiting frame, link change and grandmaster. With block, first sleep
// until one of those is due (the model's next timer or tick, a frame, a link
// change, the next gPTP poll).
void softfab_pump(struct softfab *sf, bool block);

// Milliseconds since softfab_open, the model's NOW_MS.
uint64_t softfab_now_ms(const struct softfab *sf);

#endif // MILAN_SOFTFAB_H
