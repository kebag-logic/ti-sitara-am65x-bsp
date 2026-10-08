// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// aaf.c - see aaf.h.

#include "aaf.h"

#include <string.h>

static void put_be(uint8_t *p, uint64_t v, unsigned bytes)
{
	for (unsigned i = bytes; i-- > 0;) {
		p[i] = (uint8_t)v;
		v >>= 8;
	}
}

static uint64_t get_be(const uint8_t *p, unsigned bytes)
{
	uint64_t v = 0;
	for (unsigned i = 0; i < bytes; ++i) {
		v = (v << 8) | p[i];
	}
	return v;
}

size_t aaf_build(uint8_t *out, size_t cap, const struct aaf_params *p, uint8_t seq, bool tv,
		 uint32_t avtp_timestamp, const int32_t *samples)
{
	size_t data_len = (size_t)p->frames * p->channels * 4u;
	size_t eth = AAF_ETH_BYTES + (p->vlan_tci != 0u ? AAF_VLAN_BYTES : 0u);
	size_t len = eth + AAF_HEADER_BYTES + data_len;
	if (len > cap || data_len > 0xFFFFu || p->channels > 1023u) {
		return 0;
	}
	memcpy(out, p->dst, 6);
	memcpy(out + 6, p->src, 6);
	if (p->vlan_tci != 0u) {
		put_be(out + 12, 0x8100u, 2);
		put_be(out + 14, p->vlan_tci, 2);
	}
	put_be(out + eth - 2, AAF_ETHERTYPE, 2);
	uint8_t *h = out + eth;
	h[0] = AAF_SUBTYPE;
	h[1] = (uint8_t)(0x80u | (tv ? 0x01u : 0u));    // sv = 1, version 0
	h[2] = seq;
	h[3] = 0;
	put_be(h + 4, p->stream_id, 8);
	put_be(h + 12, avtp_timestamp, 4);
	h[16] = AAF_FORMAT_INT_32BIT;
	h[17] = (uint8_t)((AAF_NSR_48KHZ << 4) | ((p->channels >> 8) & 0x03u));
	h[18] = (uint8_t)p->channels;
	h[19] = 32;
	put_be(h + 20, data_len, 2);
	h[22] = 0;                                      // sp = 0: every PDU timestamped
	h[23] = 0;
	uint8_t *d = h + AAF_HEADER_BYTES;
	size_t count = (size_t)p->frames * p->channels;
	for (size_t i = 0; i < count; ++i) {
		put_be(d + 4u * i, (uint32_t)samples[i], 4);
	}
	return len;
}

bool aaf_parse(const uint8_t *frame, size_t len, struct aaf_pdu *out)
{
	size_t eth = AAF_ETH_BYTES;
	out->vlan_tci = 0;
	if (len >= AAF_ETH_BYTES + AAF_VLAN_BYTES && get_be(frame + 12, 2) == 0x8100u) {
		out->vlan_tci = (uint16_t)get_be(frame + 14, 2);
		eth += AAF_VLAN_BYTES;
	}
	if (len < eth + AAF_HEADER_BYTES || get_be(frame + eth - 2, 2) != AAF_ETHERTYPE) {
		return false;
	}
	const uint8_t *h = frame + eth;
	if (h[0] != AAF_SUBTYPE || ((h[1] >> 4) & 0x07u) != 0u) {
		return false;
	}
	out->sv = (h[1] & 0x80u) != 0u;
	out->tv = (h[1] & 0x01u) != 0u;
	out->seq = h[2];
	out->stream_id = get_be(h + 4, 8);
	out->avtp_timestamp = (uint32_t)get_be(h + 12, 4);
	out->format = h[16];
	out->nsr = (uint8_t)(h[17] >> 4);
	out->channels = ((unsigned)(h[17] & 0x03u) << 8) | h[18];
	out->bit_depth = h[19];
	out->data_len = (uint16_t)get_be(h + 20, 2);
	out->data = h + AAF_HEADER_BYTES;
	return eth + AAF_HEADER_BYTES + out->data_len <= len;
}

void aaf_samples_to_host(int32_t *dst, const uint8_t *src, size_t count)
{
	for (size_t i = 0; i < count; ++i) {
		dst[i] = (int32_t)(uint32_t)get_be(src + 4u * i, 4);
	}
}
