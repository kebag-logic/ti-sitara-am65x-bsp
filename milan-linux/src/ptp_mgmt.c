// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// ptp_mgmt.c - GET requests to ptp4l's management socket (see ptp_mgmt.h).
//
// Layout (IEEE 1588-2019 13.3 and 15.4, as linuxptp's msg.h packs it):
//   0  header, 34 bytes: tsmt, version, messageLength, domainNumber, reserved,
//      flagField[2], correction (8), reserved (4), sourcePortIdentity (10),
//      sequenceId, controlField, logMessageInterval
//   34 targetPortIdentity (10), startingBoundaryHops, boundaryHops,
//      actionField, reserved
//   48 TLV: tlvType, lengthField, managementId, data

#define _GNU_SOURCE
#include "ptp_mgmt.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PTP_MSG_MANAGEMENT 0x0Du
#define PTP_CONTROL_MANAGEMENT 0x04u
#define PTP_ACTION_GET 0u
#define PTP_ACTION_RESPONSE 2u
#define PTP_TLV_MANAGEMENT 0x0001u
#define PTP_HDR_BYTES 34u
#define PTP_MGMT_BYTES 48u
#define PTP_GET_BYTES (PTP_MGMT_BYTES + 6u)

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t get64(const uint8_t *p)
{
	return ((uint64_t)get32(p) << 32) | get32(p + 4);
}

size_t ptp_mgmt_encode_get(uint8_t *buf, uint8_t transport_specific, uint16_t seq, uint16_t management_id)
{
	memset(buf, 0, PTP_GET_BYTES);
	buf[0] = (uint8_t)((transport_specific << 4) | PTP_MSG_MANAGEMENT);
	buf[1] = 2u;                            // versionPTP
	put16(buf + 2, PTP_GET_BYTES);
	// sourcePortIdentity stays zero, as pmc's over a Unix socket; port 1 is
	// arbitrary but not the wildcard
	buf[29] = 1u;
	put16(buf + 30, seq);
	buf[32] = PTP_CONTROL_MANAGEMENT;
	buf[33] = 0x7Fu;
	memset(buf + PTP_HDR_BYTES, 0xFF, 10);  // targetPortIdentity: every clock, every port
	buf[44] = 0u;                           // startingBoundaryHops
	buf[45] = 0u;                           // boundaryHops
	buf[46] = PTP_ACTION_GET;
	put16(buf + 48, PTP_TLV_MANAGEMENT);
	put16(buf + 50, 2u);                    // lengthField: the managementId alone
	put16(buf + 52, management_id);
	return PTP_GET_BYTES;
}

bool ptp_mgmt_decode(const uint8_t *msg, size_t len, uint16_t *management_id, uint8_t *domain,
		     const uint8_t **data, size_t *data_len)
{
	if (len < PTP_GET_BYTES || (msg[0] & 0x0Fu) != PTP_MSG_MANAGEMENT ||
	    (msg[46] & 0x0Fu) != PTP_ACTION_RESPONSE) {
		return false;
	}
	size_t msg_len = get16(msg + 2);
	if (msg_len > len || msg_len < PTP_GET_BYTES || get16(msg + 48) != PTP_TLV_MANAGEMENT) {
		return false;
	}
	size_t tlv_len = get16(msg + 50);
	if (tlv_len < 2u || PTP_MGMT_BYTES + 4u + tlv_len > msg_len) {
		return false;
	}
	*management_id = get16(msg + 52);
	*domain = msg[4];
	*data = msg + 54;
	*data_len = tlv_len - 2u;
	return true;
}

bool ptp_parse_time_status(const uint8_t *data, size_t len, struct ptp_time_status *out)
{
	// master_offset (8), ingress_time (8), cumulativeScaledRateOffset (4),
	// scaledLastGmPhaseChange (4), gmTimeBaseIndicator (2),
	// lastGmPhaseChange (ScaledNs, 12), gmPresent (4), gmIdentity (8)
	if (len < 50u) {
		return false;
	}
	out->master_offset = (int64_t)get64(data);
	out->gm_present = get32(data + 38) != 0u;
	out->gm_identity = get64(data + 42);
	return true;
}

bool ptp_parse_port_ds_np(const uint8_t *data, size_t len, struct ptp_port_ds_np *out)
{
	if (len < 8u) {
		return false;
	}
	out->neighbor_prop_delay_thresh = get32(data);
	out->as_capable = get32(data + 4) != 0u;
	return true;
}

int ptp_mgmt_open(struct ptp_mgmt *p, const char *server, const char *local, uint8_t transport_specific)
{
	memset(p, 0, sizeof *p);
	p->fd = -1;
	if (strlen(server) >= sizeof p->server.sun_path || strlen(local) >= sizeof p->local) {
		return -ENAMETOOLONG;
	}
	p->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (p->fd < 0) {
		return -errno;
	}
	struct sockaddr_un me = {.sun_family = AF_UNIX};
	strcpy(me.sun_path, local);
	unlink(local);
	if (bind(p->fd, (struct sockaddr *)&me, sizeof me) != 0) {
		int err = -errno;
		close(p->fd);
		p->fd = -1;
		return err;
	}
	strcpy(p->local, local);
	p->server.sun_family = AF_UNIX;
	strcpy(p->server.sun_path, server);
	p->transport_specific = transport_specific;
	return 0;
}

void ptp_mgmt_close(struct ptp_mgmt *p)
{
	if (p->fd >= 0) {
		close(p->fd);
		unlink(p->local);
	}
	p->fd = -1;
}

int ptp_mgmt_get(struct ptp_mgmt *p, uint16_t management_id)
{
	uint8_t buf[PTP_GET_BYTES];
	size_t n = ptp_mgmt_encode_get(buf, p->transport_specific, p->seq++, management_id);
	if (sendto(p->fd, buf, n, 0, (struct sockaddr *)&p->server, sizeof p->server) < 0) {
		return -errno;
	}
	return 0;
}
