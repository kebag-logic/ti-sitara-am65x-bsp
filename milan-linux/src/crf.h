// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// crf.h - Clock Reference Format PDUs (IEEE 1722-2016 10.4, Figure 72), as
// the Avnu Pro Audio CRF Media Clock Stream Format carries them (Milan v1.2
// 7.3.2, Table 7.1: CRF_AUDIO_SAMPLE, 48 000 Hz, pull 0, timestamp_interval
// 96, one timestamp a PDU; the AEM format 0x041060010000BB80).
//
//   byte 0      subtype 0x04 (CRF)
//   byte 1      sv (bit 7), version (6..4), mr (3), r (2), fs (1), tu (0)
//   byte 2      sequence_num
//   byte 3      type (1 = CRF_AUDIO_SAMPLE)
//   4..11       stream_id
//   12..15      pull (31..29), base_frequency (28..0)
//   16..17      crf_data_length (octets, a multiple of 8)
//   18..19      timestamp_interval (events between timestamps)
//   20..        crf_data: 64-bit gPTP times in ns, each conceptually an
//               avtp_timestamp (10.4.13.1), big-endian

#ifndef MILAN_CRF_H
#define MILAN_CRF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CRF_SUBTYPE 0x04u
#define CRF_HEADER_BYTES 20u
#define CRF_TYPE_AUDIO_SAMPLE 0x01u
#define CRF_MAX_TIMESTAMPS 16u

// The Milan format, field by field (Milan v1.2 Table 7.1)
#define CRF_MILAN_BASE_FREQUENCY 48000u
#define CRF_MILAN_PULL 0u
#define CRF_MILAN_INTERVAL 96u
#define CRF_MILAN_TIMESTAMPS 1u
#define CRF_MILAN_FORMAT 0x041060010000BB80ull

struct crf_pdu {
	uint16_t vlan_tci;              // 0 when the frame carried no tag
	bool sv, mr, fs, tu;
	uint8_t seq;
	uint8_t type;
	uint64_t stream_id;
	uint8_t pull;
	uint32_t base_frequency;
	uint16_t data_len;
	uint16_t timestamp_interval;
	unsigned n_timestamps;          // data_len / 8, at most CRF_MAX_TIMESTAMPS read
	uint64_t timestamps[CRF_MAX_TIMESTAMPS];
};

// Parse an Ethernet frame, 802.1Q-tagged or not. False for anything that is
// not a complete CRF PDU of version 0 whose crf_data_length is a multiple of 8.
bool crf_parse(const uint8_t *frame, size_t len, struct crf_pdu *out);

// Whether a PDU is in the Milan CRF format, the one STREAM_INPUT 1 runs.
bool crf_is_milan(const struct crf_pdu *p);

// The other way (the tests' talker): one frame, untagged unless vlan_tci, with
// p's header and timestamps. Returns its length, 0 when `cap` is too small.
size_t crf_build(uint8_t *out, size_t cap, const uint8_t dst[6], const uint8_t src[6], const struct crf_pdu *p);

#endif // MILAN_CRF_H
