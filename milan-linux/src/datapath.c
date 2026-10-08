// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// datapath.c - the datapath block in POSIX shared memory (see datapath.h).

#define _GNU_SOURCE
#include "datapath.h"

#include <fcntl.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static _Atomic uint32_t *seq_of(const struct milan_dp *dp)
{
	return (_Atomic uint32_t *)(uintptr_t)&dp->seq;
}

struct milan_dp *milan_dp_create(const char *name)
{
	int fd = shm_open(name, O_RDWR | O_CREAT, 0644);
	if (fd < 0) {
		return NULL;
	}
	if (ftruncate(fd, sizeof(struct milan_dp)) != 0) {
		close(fd);
		return NULL;
	}
	struct milan_dp *dp = mmap(NULL, sizeof *dp, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (dp == MAP_FAILED) {
		return NULL;
	}
	// a block a previous writer left behind is reset under the counter, so a
	// reader never takes its stale streams for current ones
	uint32_t seq = atomic_load_explicit(seq_of(dp), memory_order_relaxed) | 1u;
	atomic_store_explicit(seq_of(dp), seq, memory_order_relaxed);
	atomic_thread_fence(memory_order_release);
	size_t head = offsetof(struct milan_dp, writer_pid);
	memset((char *)dp + head, 0, sizeof *dp - head);
	dp->magic = MILAN_DP_MAGIC;
	dp->version = MILAN_DP_VERSION;
	dp->writer_pid = (uint32_t)getpid();
	atomic_store_explicit(seq_of(dp), seq + 1u, memory_order_release);
	return dp;
}

const struct milan_dp *milan_dp_open(const char *name)
{
	int fd = shm_open(name, O_RDONLY, 0);
	if (fd < 0) {
		return NULL;
	}
	struct stat st;
	if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(struct milan_dp)) {
		close(fd);
		return NULL;
	}
	const struct milan_dp *dp = mmap(NULL, sizeof *dp, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (dp == MAP_FAILED) {
		return NULL;
	}
	if (dp->magic != MILAN_DP_MAGIC || dp->version != MILAN_DP_VERSION) {
		munmap((void *)(uintptr_t)dp, sizeof *dp);
		return NULL;
	}
	return dp;
}

void milan_dp_begin(struct milan_dp *dp)
{
	uint32_t seq = atomic_load_explicit(seq_of(dp), memory_order_relaxed);
	atomic_store_explicit(seq_of(dp), seq + 1u, memory_order_relaxed);
	atomic_thread_fence(memory_order_release);
}

void milan_dp_end(struct milan_dp *dp)
{
	uint32_t seq = atomic_load_explicit(seq_of(dp), memory_order_relaxed);
	atomic_store_explicit(seq_of(dp), seq + 1u, memory_order_release);
}

bool milan_dp_snapshot(const struct milan_dp *dp, struct milan_dp *out)
{
	for (int tries = 0; tries < 1000; ++tries) {
		uint32_t before = atomic_load_explicit(seq_of(dp), memory_order_acquire);
		if (before & 1u) {
			continue;
		}
		memcpy(out, (const void *)dp, sizeof *out);
		atomic_thread_fence(memory_order_acquire);
		uint32_t after = atomic_load_explicit(seq_of(dp), memory_order_relaxed);
		if (before == after) {
			return true;
		}
	}
	return false;
}
