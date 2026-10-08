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
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "nvm_shape.h"

#define BLOCK_BYTES 65536u              // the flash's erase block, one slot
#define QUEUE 64u

uint32_t milan_nvm_entity_id_lo, milan_nvm_entity_id_hi;
uint32_t milan_nvm_model_id_lo, milan_nvm_model_id_hi;

struct range {
	uint32_t off;
	uint32_t len;
};

static struct {
	int fd;
	uint8_t image[MILAN_FLASH_JOURNAL_SIZE];
	pthread_t worker;
	pthread_mutex_t mu;
	pthread_cond_t cv;
	struct range queue[QUEUE];
	unsigned head, tail;            // tail - head operations waiting
	bool writing;                   // the worker holds one it has not synced
	bool stop;
	unsigned long write_errors;
} nf = {.fd = -1, .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

// The journal offset of a device address range, or -1 outside the journal.
static long journal_off(uint32_t addr, uint32_t len)
{
	if (addr < MILAN_FLASH_JOURNAL_OFFSET || len > MILAN_FLASH_JOURNAL_SIZE ||
	    addr - MILAN_FLASH_JOURNAL_OFFSET > MILAN_FLASH_JOURNAL_SIZE - len) {
		return -1;
	}
	return (long)(addr - MILAN_FLASH_JOURNAL_OFFSET);
}

static int enqueue(uint32_t off, uint32_t len)
{
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
	if (off < 0) {
		return -1;
	}
	pthread_mutex_lock(&nf.mu);
	memcpy(dst, nf.image + off, len);
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

int nvm_file_open(const char *path, struct nvm_flash *port)
{
	nf.fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (nf.fd < 0) {
		return -errno;
	}
	struct stat st;
	if (fstat(nf.fd, &st) != 0) {
		return -errno;
	}
	if (st.st_size == 0) {
		// a new journal is erased flash
		memset(nf.image, NVM_ERASED, sizeof nf.image);
		if (pwrite(nf.fd, nf.image, sizeof nf.image, 0) != (ssize_t)sizeof nf.image || fsync(nf.fd) != 0) {
			return -errno;
		}
	} else if (st.st_size != (off_t)sizeof nf.image ||
		   pread(nf.fd, nf.image, sizeof nf.image, 0) != (ssize_t)sizeof nf.image) {
		return -EINVAL;
	}
	int rc = pthread_create(&nf.worker, NULL, writer, NULL);
	if (rc != 0) {
		return -rc;
	}
	*port = (struct nvm_flash){port_read, port_program, port_erase, port_busy, port_now_us, NULL};
	return 0;
}

void nvm_file_close(void)
{
	if (nf.fd < 0) {
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
