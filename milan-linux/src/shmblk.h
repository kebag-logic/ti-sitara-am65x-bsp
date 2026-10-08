// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// shmblk.h - a block in POSIX shared memory with one writer and any number of
// readers. It starts with struct shmblk_hdr. The writer brackets every update
// with shmblk_begin()/shmblk_end(), which make the sequence counter odd, then
// even again; a reader copies the block and retries until it reads the same
// even value before and after.

#ifndef MILAN_SHMBLK_H
#define MILAN_SHMBLK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct shmblk_hdr {
	uint32_t magic;
	uint32_t version;
	uint32_t seq;
	uint32_t writer_pid;
};

// Writer: create (or take over) the block, zeroed under the counter, header set.
void *shmblk_create(const char *name, size_t size, uint32_t magic, uint32_t version);
// Reader: map an existing block read-only; NULL when absent or another layout.
const void *shmblk_open(const char *name, size_t size, uint32_t magic, uint32_t version);
void shmblk_begin(void *blk);
void shmblk_end(void *blk);
bool shmblk_snapshot(const void *blk, void *out, size_t size);

#endif // MILAN_SHMBLK_H
