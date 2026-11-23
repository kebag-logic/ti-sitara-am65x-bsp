// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// aecp_far.h - the datapath behind the mailbox's face block (contract 2.3) on
// Linux: what the RISC-V end station's fabric answers to AECP's face
// (milan-fpga sw/firmware/ctrl/aecp/aecp_face.h), answered here from the
// system, through the mailbox model's far side (host/mbx_model.h,
// mbx_model_far). The firmware's AECP, its face and its bridge are unchanged.
//
//   window / kind                  answered from
//   -----------------------------  ---------------------------------------------
//   GSI kind 0, STREAM_OUTPUT      the datapath block: the stream's ID, its MAAP
//                                  address once MAAP holds one, its VLAN
//   GSI kind 0, STREAM_INPUT       nothing of its own: the bridge takes the
//                                  stream, flags and VLAN from ACMP's view
//                                  (ctrl_app_aecp.c), and no MSRP runs here
//   GSI kind 0, selector 15        SET_STREAM_FORMAT's verdict: supported only
//                                  for the one format the media plane runs,
//                                  each stream's generated current_format
//   GSI kind 1 (GET_AVB_INFO)      flexptpd: the grandmaster, the mean link
//                                  delay, asCapable; gPTP enabled while its
//                                  status block is read; SRP not enabled (#14);
//                                  SR class A at PCP 3 on the talker's VLAN
//   GSI kind 2 (GET_AS_PATH)       flexptpd, where it determines the path: the
//                                  board alone as grandmaster, or the
//                                  grandmaster then the board one step from
//                                  it; anywhere else the window does not answer
//   CTR (GET_COUNTERS)             AVB_INTERFACE: LINK_UP, LINK_DOWN and
//                                  GPTP_GM_CHANGED, counted here; CLOCK_DOMAIN:
//                                  LOCKED and UNLOCKED, flexptpd's lock to the
//                                  grandmaster the media clock follows, while
//                                  its status is live (softfab.c poll_gptp);
//                                  STREAM_INPUT 0 (the AAF listener), 1 (the
//                                  CRF receive path) and STREAM_OUTPUT 0 (the
//                                  talker): the Milan counters milan-mediad
//                                  keeps for them (media.h), valid as it marks
//                                  them; any other stream: none valid
//   EDIT (ADD/REMOVE_AUDIO_MAPPINGS) refused at its begin: the media plane renders
//                                  the identity map and has no channel router,
//                                  so AECP answers NOT_SUPPORTED
//   AMAP                           never asked by the face
//   F_COMMIT                       acknowledged at once: the settings it
//                                  publishes have no consumer here
//
// A counter that moves raises the face's COUNTERS_CHANGED for its descriptor,
// and a grandmaster, asCapable or link change raises AVB_INFO_CHANGED and
// AS_PATH_CHANGED, so AECP sends its unsolicited notifications (Milan v1.2
// 5.4.5.2, Table 5.22). The media block is read every aecp_far_poll.

#ifndef MILAN_AECP_FAR_H
#define MILAN_AECP_FAR_H

#include <stdbool.h>
#include <stdint.h>

#include "datapath.h"
#include "media.h"
#include "softfab.h"

#define AECP_FAR_STREAMS 8u
#define AECP_FAR_COUNTED 2u     // the streams a media program can count (aecp_far.c stream_poll)

struct aecp_far_config {
	const char *media_name;                     // the media block's shm_open() name
	uint16_t vlan;                              // the talker's streams' VLAN (SR class A)
	uint8_t class_a_priority;                   // their PCP
	unsigned n_inputs, n_outputs;
	uint64_t input_format[AECP_FAR_STREAMS];    // each STREAM_INPUT's generated current_format
	uint64_t output_format[AECP_FAR_STREAMS];
};

struct aecp_far_counters {
	uint32_t valid;
	uint32_t value[32];
};

struct aecp_far {
	struct aecp_far_config cfg;
	struct softfab *fab;
	const struct milan_dp *dp;
	const struct milan_media *media;            // NULL until milan-mediad publishes it
	struct milan_media media_copy;
	bool media_valid;
	bool link_up, gptp_locked, as_capable;
	uint64_t gm_id;
	uint32_t link_ups, link_downs, gm_changes, locks, unlocks;
	struct aecp_far_counters input[AECP_FAR_COUNTED], output[AECP_FAR_COUNTED]; // the last poll's
	uint64_t next_media_ms;
	uint32_t answers, silent, commits, refused_edits;
};

// Bind the far side to fab's mailbox model. dp and fab outlive it.
void aecp_far_init(struct aecp_far *f, struct softfab *fab, const struct milan_dp *dp,
		   const struct aecp_far_config *cfg);
// From the main loop: read the media block (every 100 ms) and raise the face
// events of whatever moved since the last call.
void aecp_far_poll(struct aecp_far *f);

#endif // MILAN_AECP_FAR_H
