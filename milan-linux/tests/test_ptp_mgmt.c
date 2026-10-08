// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// test_ptp_mgmt.c - the management client's encoding against the layout
// linuxptp's pmc sends, and its decoding of ptp4l's RESPONSEs.

#include <stdio.h>
#include <string.h>

#include "ptp_mgmt.h"

static int failures;

#define CHECK(cond)                                                              \
	do {                                                                     \
		if (!(cond)) {                                                   \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
			failures++;                                              \
		}                                                                \
	} while (0)

static void put64(uint8_t *p, uint64_t v)
{
	for (int i = 7; i >= 0; --i) {
		p[i] = (uint8_t)v;
		v >>= 8;
	}
}

// A RESPONSE the way ptp4l builds it: the request's header and body with
// actionField RESPONSE, then one MANAGEMENT TLV carrying `len` data bytes.
static size_t response(uint8_t *buf, uint16_t id, uint8_t domain, const uint8_t *data, size_t len)
{
	size_t n = ptp_mgmt_encode_get(buf, 1, 7, id);
	buf[4] = domain;
	buf[46] = 2;
	size_t total = 54 + len;
	buf[2] = (uint8_t)(total >> 8);
	buf[3] = (uint8_t)total;
	buf[50] = (uint8_t)((2 + len) >> 8);
	buf[51] = (uint8_t)(2 + len);
	memcpy(buf + 54, data, len);
	(void)n;
	return total;
}

int main(void)
{
	uint8_t buf[256];

	// GET TIME_STATUS_NP, transportSpecific 1, as `pmc -t 1` sends it
	size_t n = ptp_mgmt_encode_get(buf, 1, 0x1234, PTP_MID_TIME_STATUS_NP);
	CHECK(n == 54);
	CHECK(buf[0] == 0x1D);                          // transportSpecific 1, management
	CHECK(buf[1] == 2);                             // versionPTP 2
	CHECK(buf[2] == 0 && buf[3] == 54);             // messageLength
	CHECK(buf[30] == 0x12 && buf[31] == 0x34);      // sequenceId
	CHECK(buf[32] == 4);                            // controlField: management
	for (int i = 34; i < 44; ++i) {
		CHECK(buf[i] == 0xFF);                  // wildcard target port
	}
	CHECK((buf[46] & 0x0F) == 0);                   // GET
	CHECK(buf[48] == 0 && buf[49] == 1);            // MANAGEMENT TLV
	CHECK(buf[50] == 0 && buf[51] == 2);            // lengthField: the id alone
	CHECK(buf[52] == 0xC0 && buf[53] == 0x00);      // TIME_STATUS_NP

	// our own GET is not a RESPONSE
	uint16_t id;
	uint8_t domain;
	const uint8_t *data;
	size_t len;
	CHECK(!ptp_mgmt_decode(buf, n, &id, &domain, &data, &len));

	// a TIME_STATUS_NP RESPONSE
	uint8_t ts[50];
	memset(ts, 0, sizeof ts);
	put64(ts, (uint64_t)(int64_t)-42);              // master_offset
	ts[41] = 1;                                     // gmPresent
	put64(ts + 42, 0x3CC0C6FFFE020100ull);          // gmIdentity
	n = response(buf, PTP_MID_TIME_STATUS_NP, 0, ts, sizeof ts);
	CHECK(ptp_mgmt_decode(buf, n, &id, &domain, &data, &len));
	CHECK(id == PTP_MID_TIME_STATUS_NP && domain == 0 && len == 50);
	struct ptp_time_status st;
	CHECK(ptp_parse_time_status(data, len, &st));
	CHECK(st.master_offset == -42);
	CHECK(st.gm_present);
	CHECK(st.gm_identity == 0x3CC0C6FFFE020100ull);

	// truncated: the TLV claims more than the message holds
	CHECK(!ptp_mgmt_decode(buf, n - 1, &id, &domain, &data, &len));
	CHECK(!ptp_parse_time_status(data, 49, &st));

	// PORT_DATA_SET_NP: neighborPropDelayThresh 800, asCapable 1
	uint8_t pd[8] = {0, 0, 0x03, 0x20, 0, 0, 0, 1};
	n = response(buf, PTP_MID_PORT_DATA_SET_NP, 0, pd, sizeof pd);
	CHECK(ptp_mgmt_decode(buf, n, &id, &domain, &data, &len));
	struct ptp_port_ds_np ds;
	CHECK(id == PTP_MID_PORT_DATA_SET_NP && ptp_parse_port_ds_np(data, len, &ds));
	CHECK(ds.neighbor_prop_delay_thresh == 800 && ds.as_capable);

	// an error status TLV (type 2) is not a RESPONSE we use
	buf[49] = 2;
	CHECK(!ptp_mgmt_decode(buf, n, &id, &domain, &data, &len));

	printf("test_ptp_mgmt: %s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
