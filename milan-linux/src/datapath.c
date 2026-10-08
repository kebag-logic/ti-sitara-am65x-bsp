// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// datapath.c - the datapath block (see datapath.h) on shmblk.h.

#include "datapath.h"

#include <stddef.h>

#include "shmblk.h"

_Static_assert(offsetof(struct milan_dp, magic) == offsetof(struct shmblk_hdr, magic) &&
		       offsetof(struct milan_dp, seq) == offsetof(struct shmblk_hdr, seq) &&
		       offsetof(struct milan_dp, writer_pid) == offsetof(struct shmblk_hdr, writer_pid),
	       "the datapath block starts with the shmblk header");

struct milan_dp *milan_dp_create(const char *name)
{
	return shmblk_create(name, sizeof(struct milan_dp), MILAN_DP_MAGIC, MILAN_DP_VERSION);
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
