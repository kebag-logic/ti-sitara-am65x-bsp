// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// test_gptp.c - the media plane's gPTP time model (gptp_time) and the reader
// of flexptpd's status block (gptp_shm).
//
//   - the model follows a PHC running 36.9 ppm fast, measured once a second
//     with +/-200 ns of read noise: between measurements it stays within
//     250 ns of the PHC;
//   - a 1 ms step of the PHC is detected, and the model is back within 500 ns
//     two measurements later;
//   - the status block reads back what was written; another layout, or a
//     writer stuck mid-update, reads as nothing.

#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "gptp_shm.h"
#include "gptp_time.h"

static int failures;

static void check(const char *what, int ok, const char *detail)
{
	printf("[%s] %s: %s\n", ok ? "PASS" : "FAIL", what, detail);
	if (!ok) {
		failures++;
	}
}

// ---- the time model ----

#define PHC_RATE (1.0 + 36.9e-6)
#define PHC_BASE 1000000000000000000ll  // the PHC at mono 0
#define NOISE_NS 200

static int64_t phc_at(int64_t mono, int64_t step)
{
	return PHC_BASE + (int64_t)((double)mono * PHC_RATE) + step;
}

static int64_t noise(void)
{
	return (rand() % (2 * NOISE_NS + 1)) - NOISE_NS;
}

// The worst |model - PHC| at 10 points between two measurements.
static int64_t worst_between(const struct gptp_time *g, int64_t from, int64_t step)
{
	int64_t worst = 0;

	for (int k = 1; k <= 10; ++k) {
		int64_t mono = from + k * 100000000ll;
		int64_t err = gptp_at(g, mono) - phc_at(mono, step);

		if (err < 0) {
			err = -err;
		}
		if (err > worst) {
			worst = err;
		}
	}

	return worst;
}

static void test_model(void)
{
	struct gptp_time g;
	memset(&g, 0, sizeof g);
	g.phc_fd = -1;
	g.model.rate = 1.0;

	srand(1);

	// 40 measurements, one a second
	int64_t worst = 0;
	int64_t mono = 5000000000ll;

	for (int i = 0; i < 40; ++i, mono += 1000000000ll) {
		gptp_time_feed(&g, mono, phc_at(mono, 0) + noise());

		// once the window is full, between this measurement and the next
		if (i >= GPTP_RATE_WINDOW) {
			int64_t w = worst_between(&g, mono, 0);
			worst = w > worst ? w : worst;
		}
	}

	char detail[128];
	snprintf(detail, sizeof detail, "worst %lld ns (limit 250), rate %+.3f ppm (PHC %+.3f)",
		 (long long)worst, (g.model.rate - 1.0) * 1e6, (PHC_RATE - 1.0) * 1e6);
	check("model: within 250 ns of a PHC 36.9 ppm fast, between measurements", worst <= 250, detail);

	// the PHC steps by 1 ms
	int64_t step = 1000000;
	uint32_t steps = g.steps;

	gptp_time_feed(&g, mono, phc_at(mono, step) + noise());
	mono += 1000000000ll;

	snprintf(detail, sizeof detail, "steps %u -> %u, residual %lld ns", steps, g.steps, (long long)g.residual_ns);
	check("model: a 1 ms PHC step is detected", g.steps == steps + 1, detail);

	gptp_time_feed(&g, mono, phc_at(mono, step) + noise());
	mono += 1000000000ll;
	gptp_time_feed(&g, mono, phc_at(mono, step) + noise());

	worst = worst_between(&g, mono, step);
	snprintf(detail, sizeof detail, "worst %lld ns (limit 500) two measurements later", (long long)worst);
	check("model: back after the step", worst <= 500, detail);
}

// ---- the status block ----

static struct gptp_shm_block *make_block(const char *name)
{
	char path[80];
	snprintf(path, sizeof path, "/%s", name);

	int fd = shm_open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0 || ftruncate(fd, sizeof(struct gptp_shm_block)) != 0) {
		perror("shm");
		exit(2);
	}

	void *p = mmap(NULL, sizeof(struct gptp_shm_block), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);

	if (p == MAP_FAILED) {
		perror("mmap");
		exit(2);
	}

	return p;
}

static void test_shm(void)
{
	char name[64];
	snprintf(name, sizeof name, "flexptpd-test-gptp-%d", (int)getpid());

	struct gptp_shm_block *b = make_block(name);

	memcpy(b->magic, GPTP_SHM_MAGIC, sizeof GPTP_SHM_MAGIC);
	b->layout = GPTP_SHM_LAYOUT;
	b->size = sizeof(struct gptp_status);
	b->seq = 2;
	b->status.port_state = GPTP_PORT_SLAVE;
	b->status.as_capable = 1;
	b->status.gm_identity = 0x3cc0c6fffefe0210ull;
	b->status.own_identity = 0x044707fffe2dbffcull;
	b->status.mean_link_delay_ns = 203;

	struct gptp_shm g;
	gptp_shm_open(&g, name);

	struct gptp_status st;
	int ok = gptp_shm_read(&g, &st) &&
		 st.gm_identity == 0x3cc0c6fffefe0210ull &&
		 st.mean_link_delay_ns == 203 &&
		 st.port_state == GPTP_PORT_SLAVE;
	check("status block: reads back what was written", ok, "SLAVE, the bench grandmaster, 203 ns");

	// no grandmaster: we are our own time base
	b->status.gm_identity = 0;
	ok = gptp_shm_read(&g, &st) && gptp_status_grandmaster(&st) == 0x044707fffe2dbffcull;
	check("status block: no grandmaster reads as our own identity", ok, "");

	// a writer stuck mid-update
	b->seq = 3;
	check("status block: an odd seq reads as nothing", !gptp_shm_read(&g, &st), "");

	// another layout, mapped afresh
	gptp_shm_close(&g);
	b->seq = 4;
	b->layout = GPTP_SHM_LAYOUT + 1;
	check("status block: another layout reads as nothing", !gptp_shm_read(&g, &st), "");

	gptp_shm_close(&g);
	munmap(b, sizeof *b);

	char path[80];
	snprintf(path, sizeof path, "/%s", name);
	shm_unlink(path);
}

int main(void)
{
	test_model();
	test_shm();

	printf("test_gptp: %s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
	return failures == 0 ? 0 : 1;
}
