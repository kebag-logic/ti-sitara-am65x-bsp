// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// milan_dp.c - print the datapath block (datapath.h) as key=value lines, so
// a shell, a test or a person can see what the control plane told the media
// plane.
//
// usage: milan-dp [shm name]

#define _GNU_SOURCE
#include <stdio.h>

#include "datapath.h"

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
	return 0;
}
