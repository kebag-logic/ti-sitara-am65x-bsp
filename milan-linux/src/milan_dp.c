// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// milan_dp.c - print the datapath block (datapath.h) as key=value lines, so
// a shell, a test or a person can see what the control plane told the media
// plane.
//
// usage: milan-dp [datapath block] [media block]

#define _GNU_SOURCE
#include <stdio.h>

#include "datapath.h"
#include "media.h"

int main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : MILAN_DP_NAME;
	const struct milan_dp *dp = milan_dp_open(name);
	if (dp == NULL) {
		fprintf(stderr, "milan-dp: no datapath block %s\n", name);
		return 1;
	}
	struct milan_dp s;
	if (!milan_dp_snapshot(dp, &s)) {
		fprintf(stderr, "milan-dp: %s is being written without end\n", name);
		return 1;
	}
	printf("writer_pid=%u\nentity_id=%016llx\nmac=%012llx\nlink_up=%u\ngm_id=%016llx\ngm_domain=%u\n",
	       s.writer_pid, (unsigned long long)s.entity_id, (unsigned long long)s.mac, s.link_up,
	       (unsigned long long)s.gm_id, s.gm_domain);
	printf("maap_valid=%u\nmaap_base=%012llx\nmaap_count=%u\n", s.maap_valid, (unsigned long long)s.maap_base,
	       s.maap_count);
	for (unsigned i = 0; i < s.n_sources && i < MILAN_DP_MAX_SOURCES; ++i) {
		const struct milan_dp_source *x = &s.sources[i];
		printf("source%u=stream_id:%016llx dest_mac:%012llx vlan:%u dest_mac_valid:%u\n", i,
		       (unsigned long long)x->stream_id, (unsigned long long)x->dest_mac, x->vlan_id,
		       x->dest_mac_valid);
	}
	for (unsigned i = 0; i < s.n_sinks && i < MILAN_DP_MAX_SINKS; ++i) {
		const struct milan_dp_sink *x = &s.sinks[i];
		printf("sink%u=stream_id:%016llx dest_mac:%012llx vlan:%u listening:%u\n", i,
		       (unsigned long long)x->stream_id, (unsigned long long)x->dest_mac, x->vlan_id, x->listening);
	}

	// milan-mediad's block, when it runs
	const struct milan_media *mb = milan_media_open(argc > 2 ? argv[2] : MILAN_MEDIA_NAME);
	struct milan_media m;
	if (mb == NULL || !milan_media_snapshot(mb, &m)) {
		return 0;
	}
	const struct milan_media_talker *t = &m.talker;
	printf("gptp_corr_ns=%lld\ngptp_residual_ns=%lld\ngptp_calibrated=%u\n", (long long)m.gptp_corr_ns,
	       (long long)m.gptp_residual_ns, m.gptp_calibrated);
	printf("talker_active=%u\ntalker_locked=%u\ntalker_pitch=%u\ntalker_stream_id=%016llx\n"
	       "talker_frames_tx=%llu\ntalker_underruns=%llu\ntalker_overruns=%llu\ntalker_late=%llu\n"
	       "talker_send_errors=%llu\ntalker_starts=%llu\ntalker_stops=%llu\ntalker_level_target=%d\n"
	       "talker_level_min=%d\ntalker_level_max=%d\ntalker_level_avg=%.3f\ntalker_late_max_ns=%d\n"
	       "talker_pto_ns=%u\n",
	       t->active, t->locked, t->pitch, (unsigned long long)t->stream_id, (unsigned long long)t->frames_tx,
	       (unsigned long long)t->underruns, (unsigned long long)t->overruns, (unsigned long long)t->late,
	       (unsigned long long)t->send_errors, (unsigned long long)t->stream_starts,
	       (unsigned long long)t->stream_stops, t->level_target, t->level_min, t->level_max,
	       t->level_avg_milli / 1000.0, t->late_max_ns, t->pto_ns);
	return 0;
}
