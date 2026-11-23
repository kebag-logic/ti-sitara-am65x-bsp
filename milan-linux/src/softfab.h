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
//   gPTP plane                   flexptpd's status block in shared memory,
//                                polled for the grandmaster and asCapable
//   interrupt and sleep          epoll on the socket and netlink, with a
//                                timeout at the next model deadline or poll
//
// One interface (MBX_N_IF is 1 in the shipping contract). One thread.

#ifndef MILAN_SOFTFAB_H
#define MILAN_SOFTFAB_H

#include <stdbool.h>
#include <stdint.h>

#include "gptp_shm.h"
#include "mbx_model.h"

// A sink's BINDING.BOUND moved in the publication block (the processor's
// acmp_bound_o; MAILBOX_CONTRACT: the firmware writes it before the BIND_RX or
// UNBIND_RX response). Called inside the firmware's write that moved it, once
// per edge, however many edges one loop pass makes; it must not call into the
// firmware.
typedef void softfab_bound_fn(void *ctx, unsigned sink, bool bound);

struct softfab_config {
	const char *ifname;
	const char *gptp_shm;           // flexptpd's status block; NULL = no gPTP plane
	unsigned gptp_poll_ms;          // its polling period
	softfab_bound_fn *bound_changed; // NULL for none
	void *bound_ctx;
};

struct softfab_stats {
	uint64_t rx_frames;             // frames handed to the model
	uint64_t rx_admitted;           // ... that it committed to a receive ring
	uint64_t tx_frames;             // frames the model sent and we transmitted
	uint64_t tx_errors;             // sendto() failures
	uint64_t tx_lost;               // frames the capture lost before we sent them
	uint64_t gm_changes;
	uint64_t gptp_reads;            // consistent status block reads
	uint64_t gptp_expiries;         // live status that went stale (softfab.c poll_gptp)
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
	struct gptp_shm gptp;
	bool gptp_open;
	uint64_t gptp_next_ms;          // the next poll
	bool gptp_counted;              // gptp_updates holds a value read from the mapped block
	uint32_t gptp_updates;          // the block's update counter, as last read
	uint64_t gptp_fresh_ms;         // when it last changed (or the mapping's baseline was read)
	struct gptp_status gptp_last;   // the last read of live status (AECP's GET_AVB_INFO, GET_AS_PATH)
	bool gptp_seen;                 // the status is live: its update counter moved within GPTP_STALE_MS
	uint32_t pub_bound;             // bit k: sink k's BINDING.BOUND on interface 0, as last seen
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
