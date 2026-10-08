// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// media.c - the media plane's status block (see media.h) on shmblk.h.

#include "media.h"

#include <stddef.h>

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
