// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// crf.c - see crf.h.

#include "crf.h"

#include <string.h>

#define ETHERTYPE_AVTP 0x22F0u
#define ETHERTYPE_VLAN 0x8100u

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

bool crf_parse(const uint8_t *frame, size_t len, struct crf_pdu *out)
{
	size_t eth = 14u;
	out->vlan_tci = 0;
	if (len >= 18u && get_be(frame + 12, 2) == ETHERTYPE_VLAN) {
		out->vlan_tci = (uint16_t)get_be(frame + 14, 2);
		eth += 4u;
	}
	if (len < eth + CRF_HEADER_BYTES || get_be(frame + eth - 2, 2) != ETHERTYPE_AVTP) {
		return false;
	}
	const uint8_t *h = frame + eth;
	if (h[0] != CRF_SUBTYPE || ((h[1] >> 4) & 0x07u) != 0u) {
		return false;
	}
	out->sv = (h[1] & 0x80u) != 0u;
	out->mr = (h[1] & 0x08u) != 0u;
	out->fs = (h[1] & 0x02u) != 0u;
	out->tu = (h[1] & 0x01u) != 0u;
	out->seq = h[2];
	out->type = h[3];
	out->stream_id = get_be(h + 4, 8);
	uint32_t pf = (uint32_t)get_be(h + 12, 4);
	out->pull = (uint8_t)(pf >> 29);
	out->base_frequency = pf & 0x1FFFFFFFu;
	out->data_len = (uint16_t)get_be(h + 16, 2);
	out->timestamp_interval = (uint16_t)get_be(h + 18, 2);
	if (out->data_len % 8u != 0u || eth + CRF_HEADER_BYTES + out->data_len > len) {
		return false;
	}
	unsigned n = out->data_len / 8u;
	out->n_timestamps = n < CRF_MAX_TIMESTAMPS ? n : CRF_MAX_TIMESTAMPS;
	for (unsigned i = 0; i < out->n_timestamps; ++i) {
		out->timestamps[i] = get_be(h + CRF_HEADER_BYTES + 8u * i, 8);
	}
	return true;
}

bool crf_is_milan(const struct crf_pdu *p)
{
	return p->sv && p->type == CRF_TYPE_AUDIO_SAMPLE && p->pull == CRF_MILAN_PULL &&
	       p->base_frequency == CRF_MILAN_BASE_FREQUENCY && p->timestamp_interval == CRF_MILAN_INTERVAL &&
	       p->data_len == 8u * CRF_MILAN_TIMESTAMPS;
}

size_t crf_build(uint8_t *out, size_t cap, const uint8_t dst[6], const uint8_t src[6], const struct crf_pdu *p)
{
	size_t eth = p->vlan_tci != 0u ? 18u : 14u;
	unsigned n = p->n_timestamps < CRF_MAX_TIMESTAMPS ? p->n_timestamps : CRF_MAX_TIMESTAMPS;
	size_t len = eth + CRF_HEADER_BYTES + 8u * n;
	if (len > cap) {
		return 0;
	}
	memcpy(out, dst, 6);
	memcpy(out + 6, src, 6);
	if (p->vlan_tci != 0u) {
		put_be(out + 12, ETHERTYPE_VLAN, 2);
		put_be(out + 14, p->vlan_tci, 2);
	}
	put_be(out + eth - 2, ETHERTYPE_AVTP, 2);
	uint8_t *h = out + eth;
	h[0] = CRF_SUBTYPE;
	h[1] = (uint8_t)((p->sv ? 0x80u : 0u) | (p->mr ? 0x08u : 0u) | (p->fs ? 0x02u : 0u) | (p->tu ? 0x01u : 0u));
	h[2] = p->seq;
	h[3] = p->type;
	put_be(h + 4, p->stream_id, 8);
	put_be(h + 12, (uint32_t)p->pull << 29 | (p->base_frequency & 0x1FFFFFFFu), 4);
	put_be(h + 16, 8u * n, 2);
	put_be(h + 18, p->timestamp_interval, 2);
	for (unsigned i = 0; i < n; ++i) {
		put_be(h + CRF_HEADER_BYTES + 8u * i, p->timestamps[i], 8);
	}
	return len;
}
