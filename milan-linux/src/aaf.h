// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// aaf.h - AVTP Audio Format PDUs (IEEE 1722-2016 7), as Milan v1.2 streams
// them: 32-bit signed integer samples, 48 kHz, a timestamp in every PDU.
//
//   byte 0      subtype 0x02 (AAF)
//   byte 1      sv (bit 7), version (6..4), mr (3), reserved, tv (0)
//   byte 2      sequence_num
//   byte 3      reserved, tu (bit 0)
//   4..11       stream_id
//   12..15      avtp_timestamp, the low 32 bits of gPTP time in ns
//   16          format (2 = INT_32BIT)
//   17..18      nsr (4 bits), reserved (2), channels_per_frame (10)
//   19          bit_depth
//   20..21      stream_data_length (bytes of samples)
//   22          reserved (3), sp (sparse timestamps, bit 4), evt (4)
//   23          reserved
//   24..        the samples, interleaved, big-endian

#ifndef MILAN_AAF_H
#define MILAN_AAF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AAF_ETHERTYPE 0x22F0u
#define AAF_SUBTYPE 0x02u
#define AAF_HEADER_BYTES 24u
#define AAF_FORMAT_INT_32BIT 0x02u
#define AAF_NSR_48KHZ 0x05u
#define AAF_ETH_BYTES 14u               // an untagged Ethernet header
#define AAF_VLAN_BYTES 4u               // an 802.1Q tag

struct aaf_params {
	uint64_t stream_id;
	uint8_t dst[6];
	uint8_t src[6];
	uint16_t vlan_tci;              // PCP << 13 | VID; 0 sends untagged
	unsigned channels;
	unsigned frames;                // sample frames per PDU (6 for class A at 48 kHz)
};

// One Ethernet frame carrying one AAF PDU: `frames` frames of `channels`
// interleaved samples, host-order int32 (ALSA S32 on the board), 802.1Q-tagged
// with vlan_tci unless it is 0. Returns its length, or 0 when `cap` is too small.
size_t aaf_build(uint8_t *out, size_t cap, const struct aaf_params *p, uint8_t seq, bool tv,
		 uint32_t avtp_timestamp, const int32_t *samples);

struct aaf_pdu {
	uint16_t vlan_tci;              // 0 when the frame carried no tag
	uint64_t stream_id;
	uint8_t seq;
	bool sv;
	bool tv;
	uint32_t avtp_timestamp;
	uint8_t format;
	uint8_t nsr;
	unsigned channels;
	uint8_t bit_depth;
	uint16_t data_len;
	const uint8_t *data;            // big-endian samples
};

// Parse an Ethernet frame, 802.1Q-tagged or not (a VLAN device hands it over
// untagged). False for anything that is not a complete AAF PDU.
bool aaf_parse(const uint8_t *frame, size_t len, struct aaf_pdu *out);

// Big-endian wire samples to host-order int32, `count` of them.
void aaf_samples_to_host(int32_t *dst, const uint8_t *src, size_t count);

#endif // MILAN_AAF_H
