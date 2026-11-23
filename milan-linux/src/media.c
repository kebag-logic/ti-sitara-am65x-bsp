// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// media.c - the media plane's status block (see media.h) on shmblk.h.

#include "media.h"

#include <stddef.h>
#include <time.h>

#include "shmblk.h"

_Static_assert(offsetof(struct milan_media, seq) == offsetof(struct shmblk_hdr, seq) &&
		       offsetof(struct milan_media, writer_pid) == offsetof(struct shmblk_hdr, writer_pid),
	       "the media block starts with the shmblk header");

struct milan_media *milan_media_create(const char *name)
{
	return shmblk_create(name, sizeof(struct milan_media), MILAN_MEDIA_MAGIC, MILAN_MEDIA_VERSION);
}

const struct milan_media *milan_media_open(const char *name)
{
	return shmblk_open(name, sizeof(struct milan_media), MILAN_MEDIA_MAGIC, MILAN_MEDIA_VERSION);
}

void milan_media_begin(struct milan_media *m)
{
	shmblk_begin(m);
}

void milan_media_end(struct milan_media *m)
{
	shmblk_end(m);
}

bool milan_media_snapshot(const struct milan_media *m, struct milan_media *out)
{
	return shmblk_snapshot(m, out, sizeof *out);
}

int64_t milan_mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void milan_counters_zero(struct milan_counters *c, struct milan_obs *o, uint32_t keep, int64_t now_ns)
{
	for (unsigned k = 0; k < MILAN_COUNTERS; ++k) {
		if (!(keep & (1u << k))) {
			c->value[k] = 0;
		}
	}
	o->hits = 0;
	o->end_ns = now_ns + MILAN_OBS_NS;
}

bool milan_obs_close(struct milan_obs *o, struct milan_counters *c, int64_t now_ns)
{
	if (now_ns >= 0 && now_ns < o->end_ns) {
		return false;
	}
	bool moved = o->hits != 0u;
	for (unsigned k = 0; k < MILAN_COUNTERS; ++k) {
		if (o->hits & (1u << k)) {
			c->value[k]++;          // 32 bits, wrapping (Milan v1.2 5.3.7.7, 5.3.8.10)
		}
	}
	o->hits = 0;
	// the next interval starts where this one ended, or now if the thread was away longer
	int64_t at = now_ns < 0 ? milan_mono_ns() : now_ns;
	o->end_ns = at - o->end_ns >= MILAN_OBS_NS || now_ns < 0 ? at + MILAN_OBS_NS : o->end_ns + MILAN_OBS_NS;
	return moved;
}
