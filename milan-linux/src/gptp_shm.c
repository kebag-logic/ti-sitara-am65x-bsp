// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// gptp_shm.c - see gptp_shm.h.

#define _GNU_SOURCE
#include "gptp_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define READ_RETRIES 8

int gptp_shm_open(struct gptp_shm *g, const char *name)
{
	memset(g, 0, sizeof *g);

	// shm_open names start with one '/'; take the name with or without it
	if (name[0] == '/') {
		name++;
	}

	if (strlen(name) >= sizeof g->name) {
		return -EINVAL;
	}

	strcpy(g->name, name);
	return 0;
}

void gptp_shm_close(struct gptp_shm *g)
{
	if (g->block != NULL) {
		munmap((void *)g->block, g->len);
	}

	g->block = NULL;
	g->len = 0;
}

// Map the block if it exists and is the layout we know.
static bool map_block(struct gptp_shm *g)
{
	char path[80];
	snprintf(path, sizeof path, "/%s", g->name);

	int fd = shm_open(path, O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0) {
		return false;
	}

	struct stat st;
	if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(struct gptp_shm_block)) {
		close(fd);
		return false;
	}

	void *p = mmap(NULL, sizeof(struct gptp_shm_block), PROT_READ, MAP_SHARED, fd, 0);
	close(fd);

	if (p == MAP_FAILED) {
		return false;
	}

	const struct gptp_shm_block *b = p;

	bool known = memcmp(b->magic, GPTP_SHM_MAGIC, sizeof GPTP_SHM_MAGIC) == 0 &&
		     b->layout == GPTP_SHM_LAYOUT &&
		     b->size == sizeof(struct gptp_status);

	if (!known) {
		munmap(p, sizeof(struct gptp_shm_block));
		return false;
	}

	g->block = b;
	g->len = sizeof(struct gptp_shm_block);
	return true;
}

bool gptp_shm_read(struct gptp_shm *g, struct gptp_status *out)
{
	if (g->block == NULL && !map_block(g)) {
		return false;
	}

	const _Atomic uint32_t *seq = (const _Atomic uint32_t *)&g->block->seq;

	for (int i = 0; i < READ_RETRIES; ++i) {
		uint32_t before = atomic_load_explicit(seq, memory_order_acquire);
		if (before & 1u) {
			continue;
		}

		memcpy(out, (const void *)&g->block->status, sizeof *out);
		atomic_thread_fence(memory_order_acquire);

		uint32_t after = atomic_load_explicit(seq, memory_order_relaxed);
		if (after == before) {
			return true;
		}
	}

	return false;
}
