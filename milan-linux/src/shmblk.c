// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// shmblk.c - see shmblk.h.

#define _GNU_SOURCE
#include "shmblk.h"

#include <fcntl.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static _Atomic uint32_t *seq_of(const void *blk)
{
	return (_Atomic uint32_t *)(uintptr_t)&((const struct shmblk_hdr *)blk)->seq;
}

void *shmblk_create(const char *name, size_t size, uint32_t magic, uint32_t version)
{
	int fd = shm_open(name, O_RDWR | O_CREAT, 0644);
	if (fd < 0) {
		return NULL;
	}
	if (ftruncate(fd, (off_t)size) != 0) {
		close(fd);
		return NULL;
	}
	void *blk = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (blk == MAP_FAILED) {
		return NULL;
	}
	// what a previous writer left is reset under the counter, so a reader never
	// takes its stale state for current
	struct shmblk_hdr *h = blk;
	uint32_t seq = atomic_load_explicit(seq_of(blk), memory_order_relaxed) | 1u;
	atomic_store_explicit(seq_of(blk), seq, memory_order_relaxed);
	atomic_thread_fence(memory_order_release);
	memset((char *)blk + offsetof(struct shmblk_hdr, writer_pid), 0,
	       size - offsetof(struct shmblk_hdr, writer_pid));
	h->magic = magic;
	h->version = version;
	h->writer_pid = (uint32_t)getpid();
	atomic_store_explicit(seq_of(blk), seq + 1u, memory_order_release);
	return blk;
}

const void *shmblk_open(const char *name, size_t size, uint32_t magic, uint32_t version)
{
	int fd = shm_open(name, O_RDONLY, 0);
	if (fd < 0) {
		return NULL;
	}
	struct stat st;
	if (fstat(fd, &st) != 0 || (size_t)st.st_size < size) {
		close(fd);
		return NULL;
	}
	const void *blk = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (blk == MAP_FAILED) {
		return NULL;
	}
	const struct shmblk_hdr *h = blk;
	if (h->magic != magic || h->version != version) {
		munmap((void *)(uintptr_t)blk, size);
		return NULL;
	}
	return blk;
}

void shmblk_begin(void *blk)
{
	uint32_t seq = atomic_load_explicit(seq_of(blk), memory_order_relaxed);
	atomic_store_explicit(seq_of(blk), seq + 1u, memory_order_relaxed);
	atomic_thread_fence(memory_order_release);
}

void shmblk_end(void *blk)
{
	uint32_t seq = atomic_load_explicit(seq_of(blk), memory_order_relaxed);
	atomic_store_explicit(seq_of(blk), seq + 1u, memory_order_release);
}

bool shmblk_snapshot(const void *blk, void *out, size_t size)
{
	for (int tries = 0; tries < 1000; ++tries) {
		uint32_t before = atomic_load_explicit(seq_of(blk), memory_order_acquire);
		if (before & 1u) {
			continue;
		}
		memcpy(out, blk, size);
		atomic_thread_fence(memory_order_acquire);
		if (atomic_load_explicit(seq_of(blk), memory_order_relaxed) == before) {
			return true;
		}
	}
	return false;
}
