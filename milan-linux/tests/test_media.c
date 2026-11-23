// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// test_media.c - the AAF PDU against IEEE 1722-2016 7 and the CRF PDU against
// its 10.4 (Figure 72) byte by byte, the Milan observation-interval counters
// (media.h), and the media clock servo in closed loop against a model of the
// USB host.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aaf.h"
#include "crf.h"
#include "media.h"
#include "servo.h"

static int failures;

#define CHECK(cond)                                                              \
	do {                                                                     \
		if (!(cond)) {                                                   \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
			failures++;                                              \
		}                                                                \
	} while (0)

static void test_aaf(void)
{
	struct aaf_params p = {
		.stream_id = 0x0200000000010000ull,
		.dst = {0x91, 0xE0, 0xF0, 0x00, 0x28, 0x24},
		.src = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01},
		.channels = 8,
		.frames = 6,
	};
	int32_t samples[48];
	for (int i = 0; i < 48; ++i) {
		samples[i] = (int32_t)(0x01020300u + (unsigned)i);
	}
	samples[47] = -2;
	uint8_t f[512];
	size_t n = aaf_build(f, sizeof f, &p, 0xAB, true, 0x11223344u, samples);
	CHECK(n == 14 + 24 + 192);
	CHECK(memcmp(f, p.dst, 6) == 0 && memcmp(f + 6, p.src, 6) == 0);
	CHECK(f[12] == 0x22 && f[13] == 0xF0);          // AVTP
	CHECK(f[14] == 0x02);                           // subtype AAF
	CHECK(f[15] == 0x81);                           // sv 1, version 0, tv 1
	CHECK(f[16] == 0xAB);                           // sequence_num
	CHECK(f[17] == 0x00);                           // tu 0
	CHECK(f[18] == 0x02 && f[25] == 0x00);          // stream_id
	CHECK(f[26] == 0x11 && f[29] == 0x44);          // avtp_timestamp
	CHECK(f[30] == 0x02);                           // INT_32BIT
	CHECK(f[31] == 0x50 && f[32] == 8);             // nsr 48 kHz, 8 channels
	CHECK(f[33] == 32);                             // bit_depth
	CHECK(f[34] == 0 && f[35] == 192);              // stream_data_length
	CHECK(f[36] == 0);                              // sp 0: every PDU timestamped
	CHECK(f[38] == 0x01 && f[39] == 0x02 && f[40] == 0x03 && f[41] == 0x00);   // big-endian
	CHECK(f[38 + 188] == 0xFF && f[38 + 191] == 0xFE);

	struct aaf_pdu pdu;
	CHECK(aaf_parse(f, n, &pdu));
	CHECK(pdu.stream_id == p.stream_id && pdu.seq == 0xAB && pdu.tv && pdu.sv);
	CHECK(pdu.avtp_timestamp == 0x11223344u && pdu.format == AAF_FORMAT_INT_32BIT);
	CHECK(pdu.nsr == AAF_NSR_48KHZ && pdu.channels == 8 && pdu.bit_depth == 32 && pdu.data_len == 192);
	int32_t back[48];
	aaf_samples_to_host(back, pdu.data, 48);
	CHECK(memcmp(back, samples, sizeof back) == 0);
	CHECK(!aaf_parse(f, n - 1, &pdu));              // short
	f[14] = 0x04;
	CHECK(!aaf_parse(f, n, &pdu));                  // CRF, not AAF
	CHECK(aaf_build(f, 100, &p, 0, true, 0, samples) == 0);

	// tagged: VID 2, PCP 3
	p.vlan_tci = (3u << 13) | 2u;
	n = aaf_build(f, sizeof f, &p, 1, true, 7, samples);
	CHECK(n == 18 + 24 + 192);
	CHECK(f[12] == 0x81 && f[13] == 0x00 && f[14] == 0x60 && f[15] == 0x02);
	CHECK(f[16] == 0x22 && f[17] == 0xF0 && f[18] == 0x02);
	CHECK(aaf_parse(f, n, &pdu) && pdu.vlan_tci == ((3u << 13) | 2u) && pdu.seq == 1 && pdu.avtp_timestamp == 7);
	CHECK(!pdu.mr && !pdu.tu);

	// mr is bit 3 of byte 1, tu bit 0 of byte 3 (IEEE 1722-2016 Figure 9)
	p.vlan_tci = 0;
	p.mr = true;
	p.tu = true;
	n = aaf_build(f, sizeof f, &p, 2, true, 9, samples);
	CHECK(f[15] == 0x89 && f[17] == 0x01);
	CHECK(aaf_parse(f, n, &pdu) && pdu.mr && pdu.tu && pdu.tv && pdu.sv);
	f[15] = 0x80;                                   // tv clear, mr clear
	f[17] = 0x00;
	CHECK(aaf_parse(f, n, &pdu) && !pdu.mr && !pdu.tu && !pdu.tv);
}

static void test_crf(void)
{
	const uint8_t dst[6] = {0x91, 0xE0, 0xF0, 0x00, 0xAB, 0x01}, src[6] = {0x02, 0, 0, 0, 0, 0x02};
	struct crf_pdu p = {
		.sv = true, .seq = 0x7F, .type = CRF_TYPE_AUDIO_SAMPLE, .stream_id = 0x0200000000020001ull,
		.pull = 0, .base_frequency = 48000, .timestamp_interval = 96, .n_timestamps = 1,
		.timestamps = {0x0102030405060708ull},
	};
	uint8_t f[256];
	size_t n = crf_build(f, sizeof f, dst, src, &p);
	CHECK(n == 14 + 20 + 8);
	CHECK(f[12] == 0x22 && f[13] == 0xF0);          // AVTP
	CHECK(f[14] == 0x04);                           // subtype CRF
	CHECK(f[15] == 0x80);                           // sv 1, version 0, mr 0, fs 0, tu 0
	CHECK(f[16] == 0x7F && f[17] == 0x01);          // sequence_num, type CRF_AUDIO_SAMPLE
	CHECK(f[18] == 0x02 && f[25] == 0x01);          // stream_id
	CHECK(f[26] == 0x00 && f[27] == 0x00 && f[28] == 0xBB && f[29] == 0x80);   // pull 0, 48000 Hz
	CHECK(f[30] == 0x00 && f[31] == 0x08);          // crf_data_length 8
	CHECK(f[32] == 0x00 && f[33] == 96);            // timestamp_interval
	CHECK(f[34] == 0x01 && f[41] == 0x08);          // the timestamp, big-endian
	struct crf_pdu q;
	CHECK(crf_parse(f, n, &q) && crf_is_milan(&q));
	CHECK(q.seq == 0x7F && q.stream_id == p.stream_id && q.n_timestamps == 1 && q.timestamps[0] == p.timestamps[0]);
	CHECK(!q.mr && !q.tu && !q.fs && q.vlan_tci == 0);
	// the Milan format string carries the same fields (Milan v1.2 Table 7.1)
	CHECK((CRF_MILAN_FORMAT >> 56) == CRF_SUBTYPE && ((CRF_MILAN_FORMAT >> 52) & 0xF) == CRF_TYPE_AUDIO_SAMPLE &&
	      ((CRF_MILAN_FORMAT >> 40) & 0xFFF) == CRF_MILAN_INTERVAL &&
	      ((CRF_MILAN_FORMAT >> 32) & 0xFF) == CRF_MILAN_TIMESTAMPS && (CRF_MILAN_FORMAT >> 29 & 7) == 0 &&
	      (CRF_MILAN_FORMAT & 0x1FFFFFFF) == CRF_MILAN_BASE_FREQUENCY);
	p.mr = p.tu = true;
	p.vlan_tci = (3u << 13) | 2u;
	n = crf_build(f, sizeof f, dst, src, &p);
	CHECK(n == 18 + 20 + 8 && f[19] == 0x89);
	CHECK(crf_parse(f, n, &q) && q.mr && q.tu && q.vlan_tci == p.vlan_tci && crf_is_milan(&q));
	CHECK(!crf_parse(f, n - 1, &q));                // short of its crf_data
	// another format: each field the input runs, changed alone
	p.vlan_tci = 0;
	struct crf_pdu other[4] = {p, p, p, p};
	other[0].base_frequency = 44100;
	other[1].timestamp_interval = 48;
	other[2].type = 2;
	other[3].pull = 1;
	for (unsigned k = 0; k < 4; ++k) {
		n = crf_build(f, sizeof f, dst, src, &other[k]);
		CHECK(crf_parse(f, n, &q) && !crf_is_milan(&q));
	}
	p.n_timestamps = 2;
	n = crf_build(f, sizeof f, dst, src, &p);
	CHECK(crf_parse(f, n, &q) && q.n_timestamps == 2 && !crf_is_milan(&q));
	f[14] = 0x02;
	CHECK(!crf_parse(f, n, &q));                    // AAF, not CRF
}

// Milan v1.2 Table 5.6: one more at the end of each interval that saw the event
static void test_obs(void)
{
	struct milan_counters c = {.valid = MILAN_SI_VALID};
	struct milan_obs o;
	c.value[MILAN_SI_MEDIA_LOCKED] = 5;
	c.value[MILAN_SI_FRAMES_RX] = 9;
	milan_counters_zero(&c, &o, 1u << MILAN_SI_MEDIA_LOCKED, 1000);
	CHECK(c.value[MILAN_SI_MEDIA_LOCKED] == 5 && c.value[MILAN_SI_FRAMES_RX] == 0 && o.end_ns == 1000 + MILAN_OBS_NS);
	for (int i = 0; i < 100; ++i) {
		milan_obs_note(&o, MILAN_SI_FRAMES_RX);             // a hundred PDUs in one interval
	}
	milan_obs_note(&o, MILAN_SI_TIMESTAMP_UNCERTAIN);
	CHECK(!milan_obs_close(&o, &c, 1000 + MILAN_OBS_NS - 1));   // not yet over
	CHECK(milan_obs_close(&o, &c, 1000 + MILAN_OBS_NS));
	CHECK(c.value[MILAN_SI_FRAMES_RX] == 1 && c.value[MILAN_SI_TIMESTAMP_UNCERTAIN] == 1 &&
	      c.value[MILAN_SI_SEQ_NUM_MISMATCH] == 0);
	CHECK(o.end_ns == 1000 + 2 * MILAN_OBS_NS && o.hits == 0);
	CHECK(!milan_obs_close(&o, &c, 1000 + 2 * MILAN_OBS_NS));  // an interval with nothing in it
	CHECK(c.value[MILAN_SI_FRAMES_RX] == 1);
	milan_obs_note(&o, MILAN_SI_FRAMES_RX);
	CHECK(milan_obs_close(&o, &c, 1000 + 10 * MILAN_OBS_NS));  // the thread away for long: it starts afresh
	CHECK(c.value[MILAN_SI_FRAMES_RX] == 2 && o.end_ns == 1000 + 11 * MILAN_OBS_NS);
	c.value[MILAN_SI_LATE_TIMESTAMP] = UINT32_MAX;
	milan_obs_note(&o, MILAN_SI_LATE_TIMESTAMP);
	CHECK(milan_obs_close(&o, &c, -1) && c.value[MILAN_SI_LATE_TIMESTAMP] == 0);   // closed at once; 32 bits wrap
}

// The host sends 48 000 * (1 + (pitch + host_ppm) * 1e-6) frames/s; the
// talker takes 48 000. The level is seen through the microframe granularity
// (+-3 frames of noise on each 125 us sample, averaged over the 50 ms update).
static void test_servo(double host_ppm)
{
	struct servo s;
	servo_init(&s, 24.0, -250000.0, 5000.0);
	double level = 0.0, out = 0.0, worst_late = 0.0, peak = 0.0;
	srand(1);
	for (int step = 0; step < 1200; ++step) {   // 60 s at 20 Hz
		double sum = 0.0;
		for (int k = 0; k < 400; ++k) {
			level += 48000.0 * (out + host_ppm) * 1e-6 * 125e-6;
			sum += level + (double)(rand() % 7 - 3);
		}
		out = servo_step(&s, sum / 400.0, 0.05);
		if (step >= 400) {                  // after 20 s
			double e = fabs(level - 24.0);
			worst_late = e > worst_late ? e : worst_late;
		}
		peak = level > peak ? level : peak;
	}
	printf("servo, host %+.0f ppm: pitch %+.2f ppm (integral %+.2f), level %.3f, worst |error| after 20 s %.3f, "
	       "peak %.1f, %s\n",
	       host_ppm, out, s.integral, level, worst_late, peak, s.locked ? "locked" : "not locked");
	CHECK(s.locked);
	CHECK(worst_late < 1.0);
	CHECK(fabs(s.integral + host_ppm) < 1.0);       // the steady state cancels the host's offset
	CHECK(peak < 96.0);                             // never reaches the drop level
}

int main(void)
{
	test_aaf();
	test_crf();
	test_obs();
	test_servo(0.0);
	test_servo(100.0);
	test_servo(-100.0);
	test_servo(300.0);
	printf("test_media: %s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
