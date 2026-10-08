// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// ptp_mgmt.h - a minimal IEEE 1588 management client for ptp4l's Unix
// socket: GET requests and their RESPONSE data, nothing else.
//
// It speaks to the read-only socket (/var/run/ptp4lro, uds_ro_address), which
// answers GET only. The messages are the ones pmc sends: a management message
// with a wildcard target port and one MANAGEMENT TLV, network byte order.
// ptp4l drops a message whose transportSpecific differs from its own, so a
// gPTP instance (transportSpecific 0x1) needs it set to 1.

#ifndef MILAN_PTP_MGMT_H
#define MILAN_PTP_MGMT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/un.h>

#define PTP_MID_TIME_STATUS_NP 0xC000u
#define PTP_MID_PORT_DATA_SET_NP 0xC002u

struct ptp_mgmt {
	int fd;
	struct sockaddr_un server;
	char local[sizeof(((struct sockaddr_un *)0)->sun_path)];
	uint8_t transport_specific;
	uint16_t seq;
};

// TIME_STATUS_NP (linuxptp): the grandmaster this clock follows.
struct ptp_time_status {
	int64_t master_offset;
	bool gm_present;        // false while this clock is its own grandmaster
	uint64_t gm_identity;   // the grandmaster's ClockIdentity, big-endian
};

// PORT_DATA_SET_NP (linuxptp): the 802.1AS port state.
struct ptp_port_ds_np {
	uint32_t neighbor_prop_delay_thresh;
	bool as_capable;
};

// Bind `local` (removed first) and aim at `server`. 0, or -errno.
int ptp_mgmt_open(struct ptp_mgmt *p, const char *server, const char *local, uint8_t transport_specific);
void ptp_mgmt_close(struct ptp_mgmt *p);

// Send one GET of `management_id`. 0, or -errno.
int ptp_mgmt_get(struct ptp_mgmt *p, uint16_t management_id);

// Encode a GET into buf (at least 54 bytes); returns its length. Exposed for
// the tests, which decode it as ptp4l does.
size_t ptp_mgmt_encode_get(uint8_t *buf, uint8_t transport_specific, uint16_t seq, uint16_t management_id);

// Decode one received RESPONSE: its management ID, domain and data. False for
// anything else (another message type, an error status TLV, a short frame).
bool ptp_mgmt_decode(const uint8_t *msg, size_t len, uint16_t *management_id, uint8_t *domain,
		     const uint8_t **data, size_t *data_len);

bool ptp_parse_time_status(const uint8_t *data, size_t len, struct ptp_time_status *out);
bool ptp_parse_port_ds_np(const uint8_t *data, size_t len, struct ptp_port_ds_np *out);

#endif // MILAN_PTP_MGMT_H
