// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// datapath.h - the datapath block: what the control plane (milan-ctrld)
// tells the media plane about the streams, in POSIX shared memory.
//
// It stands in for the fabric's datapath CSR window of the RISC-V end
// station: the MAAP allocation the talker addresses its streams to, each
// STREAM_OUTPUT's identity, and each STREAM_INPUT the listener has settled on.
// One writer (milan-ctrld), any number of readers. Every update runs under a
// sequence counter that is odd while it is written, so a reader copies the
// block and retries until it reads the same even value before and after.

#ifndef MILAN_DATAPATH_H
#define MILAN_DATAPATH_H

#include <stdbool.h>
#include <stdint.h>

#define MILAN_DP_NAME "/milan-datapath"         // shm_open() name
#define MILAN_DP_MAGIC 0x3150444Du              // "MDP1"
#define MILAN_DP_VERSION 1u
#define MILAN_DP_MAX_SOURCES 8u
#define MILAN_DP_MAX_SINKS 8u

// A STREAM_OUTPUT (talker side).
struct milan_dp_source {
	uint64_t stream_id;     // the entity's MAC, then the 16-bit unique ID
	uint64_t dest_mac;      // 48 bits, valid with dest_mac_valid
	uint16_t vlan_id;
	uint8_t dest_mac_valid; // MAAP holds an address for this stream
	uint8_t pad[5];
};

// A STREAM_INPUT (listener side).
struct milan_dp_sink {
	uint64_t stream_id;
	uint64_t dest_mac;      // 48 bits
	uint16_t vlan_id;
	uint8_t listening;      // ACMP settled: receive this stream
	uint8_t pad[5];
};

struct milan_dp {
	uint32_t magic;
	uint32_t version;
	uint32_t seq;           // odd while milan-ctrld writes
	uint32_t writer_pid;
	uint64_t entity_id;
	uint64_t mac;           // the entity's 48-bit MAC
	uint64_t gm_id;         // the gPTP grandmaster ADP advertises
	uint8_t gm_domain;
	uint8_t link_up;
	uint8_t maap_valid;
	uint8_t pad0;
	uint16_t maap_count;
	uint16_t pad1;
	uint64_t maap_base;     // 48 bits
	uint32_t n_sources;
	uint32_t n_sinks;
	struct milan_dp_source sources[MILAN_DP_MAX_SOURCES];
	struct milan_dp_sink sinks[MILAN_DP_MAX_SINKS];
};

// Writer: create (or take over) the block and map it; NULL on failure.
struct milan_dp *milan_dp_create(const char *name);
// Reader: map an existing block read-only; NULL when absent or of another
// layout.
const struct milan_dp *milan_dp_open(const char *name);
// Writer: bracket every update.
void milan_dp_begin(struct milan_dp *dp);
void milan_dp_end(struct milan_dp *dp);
// Reader: a consistent copy of the whole block; false after too many retries.
bool milan_dp_snapshot(const struct milan_dp *dp, struct milan_dp *out);

#endif // MILAN_DATAPATH_H
