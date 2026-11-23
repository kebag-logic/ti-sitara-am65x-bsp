// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// datapath.c - the datapath block (see datapath.h) on shmblk.h.

#include "datapath.h"

#include <stddef.h>
#include <sys/mman.h>

#include "shmblk.h"

_Static_assert(offsetof(struct milan_dp, magic) == offsetof(struct shmblk_hdr, magic) &&
		       offsetof(struct milan_dp, seq) == offsetof(struct shmblk_hdr, seq) &&
		       offsetof(struct milan_dp, writer_pid) == offsetof(struct shmblk_hdr, writer_pid),
	       "the datapath block starts with the shmblk header");

struct milan_dp *milan_dp_create(const char *name)
{
	// the bind counts of the block taken over, read word by word (a writer
	// killed inside an update tears no aligned word), so a binding restored
	// at boot moves its sink's count past the one the media plane last zeroed for
	uint32_t binds[MILAN_DP_MAX_SINKS] = {0};
	const struct milan_dp *old = milan_dp_open(name);
	if (old != NULL) {
		for (unsigned k = 0; k < MILAN_DP_MAX_SINKS; ++k) {
			binds[k] = __atomic_load_n(&old->sinks[k].binds, __ATOMIC_RELAXED);
		}
		munmap((void *)(uintptr_t)old, sizeof *old);
	}
	struct milan_dp *dp = shmblk_create(name, sizeof(struct milan_dp), MILAN_DP_MAGIC, MILAN_DP_VERSION);
	if (dp != NULL) {
		milan_dp_begin(dp);
		for (unsigned k = 0; k < MILAN_DP_MAX_SINKS; ++k) {
			dp->sinks[k].binds = binds[k];
		}
		milan_dp_end(dp);
	}
	return dp;
}

const struct milan_dp *milan_dp_open(const char *name)
{
	return shmblk_open(name, sizeof(struct milan_dp), MILAN_DP_MAGIC, MILAN_DP_VERSION);
}

void milan_dp_begin(struct milan_dp *dp)
{
	shmblk_begin(dp);
}

void milan_dp_end(struct milan_dp *dp)
{
	shmblk_end(dp);
}

bool milan_dp_snapshot(const struct milan_dp *dp, struct milan_dp *out)
{
	return shmblk_snapshot(dp, out, sizeof *out);
}
