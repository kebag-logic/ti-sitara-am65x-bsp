// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// milan_dp.c - print the datapath block (datapath.h) as key=value lines, so
// a shell, a test or a person can see what the control plane told the media
// plane; or, with -g, flexptpd's gPTP status block (gptp_shm.h).
//
// usage: milan-dp [datapath block] [media block]
//        milan-dp -g [gPTP status block, flexptpd.eth0]

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "datapath.h"
#include "gptp_shm.h"
#include "media.h"

static const char *const PORT_STATES[] = {
	"INITIALIZING", "LISTENING", "PRE_MASTER", "MASTER", "SLAVE",
	"PASSIVE", "UNCALIBRATED", "FAULTY", "DISABLED",
};

static int print_gptp(const char *name)
{
	struct gptp_shm g;
	if (gptp_shm_open(&g, name) != 0) {
		fprintf(stderr, "milan-dp: bad status block name %s\n", name);
		return 1;
	}

	struct gptp_status s;
	if (!gptp_shm_read(&g, &s)) {
		fprintf(stderr, "milan-dp: no gPTP status block %s (is flexptpd running?)\n", name);
		return 1;
	}

	const char *state = s.port_state < sizeof PORT_STATES / sizeof PORT_STATES[0] ? PORT_STATES[s.port_state] : "?";

	printf("port_state=%s\nlink_up=%u\nas_capable=%u\nis_measuring_delay=%u\ndomain=%u\n",
	       state, s.link_up, s.as_capable, s.is_measuring_delay, s.domain);
	printf("gm_present=%u\nlocked=%u\nown_identity=%016llx\ngm_identity=%016llx\n",
	       s.gm_present, s.locked, (unsigned long long)s.own_identity, (unsigned long long)s.gm_identity);
	printf("gm_priority1=%u\ngm_clock_class=%u\ngm_clock_accuracy=0x%02x\ngm_variance=0x%04x\ngm_priority2=%u\n",
	       s.gm_priority1, s.gm_clock_class, s.gm_clock_accuracy, s.gm_variance, s.gm_priority2);
	printf("steps_removed=%u\ngm_time_base_indicator=%u\ngm_changes=%u\n",
	       s.steps_removed, s.gm_time_base_indicator, s.gm_changes);
	printf("mean_link_delay_ns=%lld\nneighbor_rate_ratio=%.9f\nrate_ratio=%.9f\ntime_error_ns=%lld\ntuning_ppb=%.3f\n",
	       (long long)s.mean_link_delay_ns, s.neighbor_rate_ratio, s.rate_ratio, (long long)s.time_error_ns,
	       s.tuning_ppb);
	printf("log_sync_interval=%d\nlog_pdelay_interval=%d\nlog_announce_interval=%d\n",
	       s.log_sync_interval, s.log_pdelay_interval, s.log_announce_interval);
	printf("sync_rx=%u\nsync_timeouts=%u\nannounce_rx=%u\nannounce_timeouts=%u\npdelay_lost=%u\n"
	       "pdelay_multiple=%u\nsignaling_rx=%u\ntx_timestamps_lost=%u\n",
	       s.sync_rx, s.sync_timeouts, s.announce_rx, s.announce_timeouts, s.pdelay_lost, s.pdelay_multiple,
	       s.signaling_rx, s.tx_timestamps_lost);
	printf("rsync_enabled=%u\nrsync_domain=%u\nrsync_interval_us=%u\nrsync_sent=%u\nupdates=%u\n",
	       s.rsync_enabled, s.rsync_domain, s.rsync_interval_us, s.rsync_sent, s.updates);

	gptp_shm_close(&g);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "-g") == 0) {
		return print_gptp(argc > 2 ? argv[2] : "flexptpd.eth0");
	}

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
	printf("gptp_rate_ppb=%lld\ngptp_residual_ns=%lld\ngptp_calibrated=%u\n", (long long)m.gptp_rate_ppb,
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
	const struct milan_media_listener *l = &m.listener;
	printf("listener_active=%u\nlistener_locked=%u\nlistener_pitch=%u\nlistener_stream_id=%016llx\n"
	       "listener_frames_rx=%llu\nlistener_seq_mismatch=%llu\nlistener_late_timestamp=%llu\n"
	       "listener_early_timestamp=%llu\nlistener_unsupported_format=%llu\nlistener_media_locked=%llu\n"
	       "listener_media_unlocked=%llu\nlistener_media_resets=%llu\nlistener_stream_interrupted=%llu\n"
	       "listener_underruns=%llu\nlistener_align_min_ns=%d\nlistener_align_max_ns=%d\n"
	       "listener_align_avg_ns=%d\nlistener_margin_min_ns=%d\nlistener_in_flight_ns=%lld\n",
	       l->active, l->locked, l->pitch, (unsigned long long)l->stream_id, (unsigned long long)l->frames_rx,
	       (unsigned long long)l->seq_mismatch, (unsigned long long)l->late_timestamp,
	       (unsigned long long)l->early_timestamp, (unsigned long long)l->unsupported_format,
	       (unsigned long long)l->media_locked, (unsigned long long)l->media_unlocked,
	       (unsigned long long)l->media_resets, (unsigned long long)l->stream_interrupted,
	       (unsigned long long)l->underruns, l->align_min_ns, l->align_max_ns, l->align_avg_ns,
	       l->margin_min_ns, (long long)l->in_flight_ns);
	return 0;
}
