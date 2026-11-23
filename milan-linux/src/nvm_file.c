// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// nvm_file.c - see nvm_file.h.

#define _GNU_SOURCE
#include "nvm_file.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "nvm_shape.h"

#define BLOCK_BYTES NVM_ERASE_BLOCK     // the flash's erase block
#define QUEUE 64u
// The journal the store writes: journal_klj3, two slots of whole erase blocks
// (nvm_shape.h NVM_JOURNAL_OFFSET/SIZE). The all-fabric placement's KLJ2
// journal (NVM_LEGACY_A, MILAN_FLASH_JOURNAL_SIZE) the store only reads, for
// its headers, and never writes (nvm_store.c, nvm_legacy_check).
#define JOURNAL_BYTES NVM_JOURNAL_SIZE
#define LEGACY_BYTES MILAN_FLASH_JOURNAL_SIZE

uint32_t milan_nvm_entity_id_lo, milan_nvm_entity_id_hi;
uint32_t milan_nvm_model_id_lo, milan_nvm_model_id_hi;

struct range {
	uint32_t off;
	uint32_t len;
};

static struct {
	int fd;
	bool volatile_only;             // no file: the journal lasts as long as the process
	uint8_t *image;                 // the KLJ3 journal's RAM image (journal_image)
	uint8_t *legacy;                // the KLJ2 journal file found at the path, else erased
	pthread_t worker;
	pthread_mutex_t mu;
	pthread_cond_t cv;
	struct range queue[QUEUE];
	unsigned head, tail;            // tail - head operations waiting
	bool writing;                   // the worker holds one it has not synced
	bool stop;
	unsigned long write_errors;
} nf = {.fd = -1, .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};
// zero-initialised, so neither is carried in the program file
static uint8_t journal_image[JOURNAL_BYTES];
static uint8_t legacy_image[LEGACY_BYTES];

// The journal offset of a device address range, or -1 outside the journal.
static long journal_off(uint32_t addr, uint32_t len)
{
	if (addr < NVM_JOURNAL_OFFSET || len > JOURNAL_BYTES || addr - NVM_JOURNAL_OFFSET > JOURNAL_BYTES - len) {
		return -1;
	}
	return (long)(addr - NVM_JOURNAL_OFFSET);
}

// The legacy journal's offset of a device address range, or -1 outside it.
static long legacy_off(uint32_t addr, uint32_t len)
{
	if (addr < NVM_LEGACY_A || len > LEGACY_BYTES || addr - NVM_LEGACY_A > LEGACY_BYTES - len) {
		return -1;
	}
	return (long)(addr - NVM_LEGACY_A);
}

static int enqueue(uint32_t off, uint32_t len)
{
	if (nf.volatile_only) {
		return 0;
	}
	if (nf.tail - nf.head >= QUEUE) {
		return -1;
	}
	nf.queue[nf.tail % QUEUE] = (struct range){off, len};
	nf.tail++;
	pthread_cond_signal(&nf.cv);
	return 0;
}

static int port_read(void *ctx, uint32_t addr, uint8_t *dst, uint32_t len)
{
	(void)ctx;
	long off = journal_off(addr, len);
	long old = legacy_off(addr, len);
	if (off < 0 && old < 0) {
		return -1;
	}
	pthread_mutex_lock(&nf.mu);
	memcpy(dst, off >= 0 ? nf.image + off : nf.legacy + old, len);
	pthread_mutex_unlock(&nf.mu);
	return 0;
}

static int port_program(void *ctx, uint32_t addr, const uint8_t *src, uint32_t len)
{
	(void)ctx;
	long off = journal_off(addr, len);
	if (off < 0 || len == 0u || len > NVM_FLASH_PAGE ||
	    (addr / NVM_FLASH_PAGE) != ((addr + len - 1u) / NVM_FLASH_PAGE)) {
		return -1;
	}
	pthread_mutex_lock(&nf.mu);
	for (uint32_t i = 0; i < len; ++i) {
		nf.image[off + i] &= src[i];            // a program only clears bits
	}
	int rc = enqueue((uint32_t)off, len);
	pthread_mutex_unlock(&nf.mu);
	return rc;
}

static int port_erase(void *ctx, uint32_t addr)
{
	(void)ctx;
	long off = journal_off(addr & ~(BLOCK_BYTES - 1u), BLOCK_BYTES);
	if (off < 0) {
		return -1;
	}
	pthread_mutex_lock(&nf.mu);
	memset(nf.image + off, NVM_ERASED, BLOCK_BYTES);
	int rc = enqueue((uint32_t)off, BLOCK_BYTES);
	pthread_mutex_unlock(&nf.mu);
	return rc;
}

static int port_busy(void *ctx)
{
	(void)ctx;
	pthread_mutex_lock(&nf.mu);
	int busy = nf.tail != nf.head || nf.writing;
	pthread_mutex_unlock(&nf.mu);
	return busy;
}

static uint64_t port_now_us(void *ctx)
{
	(void)ctx;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void *writer(void *arg)
{
	(void)arg;
	static uint8_t buf[BLOCK_BYTES];
	pthread_mutex_lock(&nf.mu);
	for (;;) {
		while (nf.tail == nf.head && !nf.stop) {
			pthread_cond_wait(&nf.cv, &nf.mu);
		}
		if (nf.tail == nf.head) {
			break;
		}
		struct range r = nf.queue[nf.head % QUEUE];
		nf.head++;
		nf.writing = true;
		memcpy(buf, nf.image + r.off, r.len);
		pthread_mutex_unlock(&nf.mu);
		if (pwrite(nf.fd, buf, r.len, r.off) != (ssize_t)r.len || fdatasync(nf.fd) != 0) {
			nf.write_errors++;
		}
		pthread_mutex_lock(&nf.mu);
		nf.writing = false;
	}
	pthread_mutex_unlock(&nf.mu);
	return NULL;
}

// A file of the KLJ2 journal's size at the path is the earlier image's journal:
// it is moved aside to <path>.klj2, read back as the legacy range and never
// written, and the KLJ3 journal starts erased at the path.
static int take_legacy(const char *path)
{
	char aside[4096];
	if ((size_t)snprintf(aside, sizeof aside, "%s.klj2", path) >= sizeof aside) {
		return -ENAMETOOLONG;
	}
	if (pread(nf.fd, nf.legacy, (size_t)LEGACY_BYTES, 0) != (ssize_t)LEGACY_BYTES) {
		return -EIO;
	}
	close(nf.fd);
	nf.fd = -1;
	if (rename(path, aside) != 0) {
		return -errno;
	}
	nf.fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	return nf.fd < 0 ? -errno : 0;
}

int nvm_file_open(const char *path, struct nvm_flash *port)
{
	nf.image = journal_image;
	nf.legacy = legacy_image;
	memset(nf.legacy, NVM_ERASED, (size_t)LEGACY_BYTES);
	*port = (struct nvm_flash){port_read, port_program, port_erase, port_busy, port_now_us, NULL};
	if (path == NULL) {
		// no media: erased flash that lives as long as the process
		nf.volatile_only = true;
		memset(nf.image, NVM_ERASED, (size_t)JOURNAL_BYTES);
		return 0;
	}
	nf.fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (nf.fd < 0) {
		return -errno;
	}
	struct stat st;
	if (fstat(nf.fd, &st) != 0) {
		return -errno;
	}
	if (st.st_size == (off_t)LEGACY_BYTES) {
		int rc = take_legacy(path);
		if (rc != 0) {
			return rc;
		}
		st.st_size = 0;
	}
	if (st.st_size == 0) {
		// a new journal is erased flash
		memset(nf.image, NVM_ERASED, (size_t)JOURNAL_BYTES);
		if (pwrite(nf.fd, nf.image, (size_t)JOURNAL_BYTES, 0) != (ssize_t)JOURNAL_BYTES || fsync(nf.fd) != 0) {
			return -errno;
		}
	} else if (st.st_size != (off_t)JOURNAL_BYTES ||
		   pread(nf.fd, nf.image, (size_t)JOURNAL_BYTES, 0) != (ssize_t)JOURNAL_BYTES) {
		return -EINVAL;
	}
	int rc = pthread_create(&nf.worker, NULL, writer, NULL);
	return rc != 0 ? -rc : 0;
}

void nvm_file_close(void)
{
	if (nf.volatile_only || nf.fd < 0) {
		return;
	}
	pthread_mutex_lock(&nf.mu);
	nf.stop = true;
	pthread_cond_signal(&nf.cv);
	pthread_mutex_unlock(&nf.mu);
	pthread_join(nf.worker, NULL);
	close(nf.fd);
	nf.fd = -1;
}
