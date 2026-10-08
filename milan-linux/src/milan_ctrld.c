// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// milan_ctrld.c - the Milan control plane of the PocketBeagle 2 USB-to-Milan
// bridge: milan-fpga's Mark II control-plane firmware, unchanged, on the soft
// fabric (softfab.h), with the integrator's ports wired to Linux.
//
// The composition is the RISC-V platform's (ctrl_app.h): ADP always, ACMP for
// the entity's STREAM_INPUTs and STREAM_OUTPUTs, and MAAP for its talker
// sources. Their integrator ports land here:
//
//   port                          here
//   ----------------------------  --------------------------------------------
//   acmp_env.locked               never locked (AECP's lock arrives with it)
//   acmp_env.source               the talker's stream: the entity MAC and the
//                                 source index as stream_id, MAAP's address
//   acmp_env.srp                  the listener's settled stream, published for
//                                 the media plane; with -n (no SRP domain: a
//                                 direct link, no MSRP bridge), its talker also
//                                 counts as registered, so the sink holds
//                                 SETTLED_RSV_OK instead of re-probing every
//                                 TMR_NO_TK
//   acmp_env.persist, .changed    logged (the saved state and AECP's
//                                 notifications arrive with their lanes)
//   maap_allocation               the MAAP range, published for the media
//                                 plane and read back by acmp_env.source
//
// What the media plane needs is in the datapath block (datapath.h).

#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "ctrl_app.h"
#include "datapath.h"
#include "entity_conf.h"
#include "softfab.h"

#define DEFAULT_VLAN 2u                 // Milan v1.2 4.2.7: the SR class default VID
#define SHUTDOWN_MS 200u                // service time left for ENTITY_DEPARTING

static struct {
	const char *ifname;
	const char *entity_path;
	const char *ptp_server;
	const char *ptp_local;
	const char *dp_name;
	uint16_t vlan;
	unsigned ptp_poll_ms;
	bool use_syslog;
	bool no_srp;
	int verbose;
} opt = {
	.ifname = "eth0",
	.entity_path = "/etc/milan/entity.conf",
	.ptp_server = "/var/run/ptp4lro",
	.ptp_local = "/var/run/milan-ctrld.ptp",
	.dp_name = MILAN_DP_NAME,
	.vlan = DEFAULT_VLAN,
	.ptp_poll_ms = 250u,
};

// sinks whose talker attribute the no-SRP mode owes the ACMP core, delivered
// from the main loop: a port never calls back into a core (milan-fpga #678)
static bool tk_owed[ACMP_MAX_SINKS];

static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t dump_requested;

static struct softfab fab;
static struct ctrl_app app;
static struct entity_conf econf;
static struct acmp_config acmp_cfg;
static struct milan_dp *dp;

// lwSRP's pool, the RV32 image's classes; the composition carves it even
// before SRP is bound. The arena also holds the alignment and a flag per block
// (ctrl_pool_arena_bytes), so it is sized with room and checked at start.
static const struct ctrl_pool_class pool_classes[] = {{32u, 8u}};
static max_align_t pool_arena[1024u / sizeof(max_align_t)];

// ---- logging -----------------------------------------------------------------

static void say(int level, const char *fmt, ...)
{
	if (level == LOG_DEBUG && opt.verbose < 1) {
		return;
	}
	va_list ap;
	va_start(ap, fmt);
	if (opt.use_syslog) {
		vsyslog(level, fmt, ap);
	} else {
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
	}
	va_end(ap);
}

// shlan_printf and the firmware's debug prints
static void debug_sink(void *ctx, const char *text, size_t len)
{
	(void)ctx;
	if (opt.verbose >= 2) {
		say(LOG_DEBUG, "fw: %.*s", (int)len, text);
	}
}

// ---- the integrator's ports ---------------------------------------------------

static bool env_locked(void *ctx, uint64_t *controller_entity_id)
{
	(void)ctx;
	*controller_entity_id = 0u;
	return false;
}

static void env_source(void *ctx, unsigned index, struct acmp_source_state *out)
{
	(void)ctx;
	memset(out, 0, sizeof *out);
	if (index < MILAN_DP_MAX_SOURCES) {
		const struct milan_dp_source *s = &dp->sources[index];
		out->dest_mac_valid = s->dest_mac_valid != 0u;
		out->stream.stream_id = s->stream_id;
		out->stream.dest_mac = s->dest_mac;
		out->stream.vlan_id = s->vlan_id;
	}
}

static void env_srp(void *ctx, unsigned sink, const struct acmp_stream *stream)
{
	(void)ctx;
	if (sink >= MILAN_DP_MAX_SINKS) {
		return;
	}
	milan_dp_begin(dp);
	struct milan_dp_sink *k = &dp->sinks[sink];
	k->listening = stream != NULL;
	k->stream_id = stream != NULL ? stream->stream_id : 0u;
	k->dest_mac = stream != NULL ? stream->dest_mac : 0u;
	k->vlan_id = stream != NULL ? stream->vlan_id : 0u;
	milan_dp_end(dp);
	if (sink < ACMP_MAX_SINKS) {
		tk_owed[sink] = opt.no_srp && stream != NULL;
	}
	if (stream != NULL) {
		say(LOG_INFO, "sink %u: listening to stream %016llx at %012llx, VLAN %u", sink,
		    (unsigned long long)stream->stream_id, (unsigned long long)stream->dest_mac, stream->vlan_id);
	} else {
		say(LOG_INFO, "sink %u: stopped", sink);
	}
}

static void env_persist(void *ctx, unsigned sink)
{
	(void)ctx;
	say(LOG_DEBUG, "sink %u: binding changed (no saved state on this build)", sink);
}

static void env_changed(void *ctx, unsigned sink)
{
	(void)ctx;
	const struct acmp_sink *k = &app.acmp.acmp.sinks[sink];
	say(LOG_INFO, "sink %u: %s, talker %016llx/%u, acmp status %u", sink, k->bound ? "bound" : "unbound",
	    (unsigned long long)k->binding.talker_entity_id, k->binding.talker_unique_id, k->acmp_status);
}

static const struct acmp_env acmp_env = {
	NULL, env_locked, env_source, env_srp, env_persist, env_changed,
};

static void maap_allocated(void *ctx, unsigned interface, uint64_t base, uint16_t count, bool valid)
{
	(void)ctx;
	(void)interface;
	milan_dp_begin(dp);
	dp->maap_valid = valid;
	dp->maap_base = base;
	dp->maap_count = count;
	for (unsigned i = 0; i < dp->n_sources; ++i) {
		struct milan_dp_source *s = &dp->sources[i];
		s->dest_mac_valid = valid && i < count;
		s->dest_mac = s->dest_mac_valid ? base + i : 0u;
	}
	milan_dp_end(dp);
	if (valid) {
		say(LOG_INFO, "MAAP: %u addresses from %012llx", count, (unsigned long long)base);
	} else {
		say(LOG_INFO, "MAAP: no addresses");
	}
}

// ---- status ------------------------------------------------------------------

static void publish_gptp(void)
{
	if (dp->gm_id != fab.gm_id || dp->gm_domain != fab.gm_domain || dp->link_up != fab.link_up) {
		milan_dp_begin(dp);
		dp->gm_id = fab.gm_id;
		dp->gm_domain = fab.gm_domain;
		dp->link_up = fab.link_up;
		milan_dp_end(dp);
		say(LOG_INFO, "link %s, grandmaster %016llx domain %u", fab.link_up ? "up" : "down",
		    (unsigned long long)fab.gm_id, fab.gm_domain);
	}
}

static void dump(void)
{
	const struct ctrl_loop_stats *l = &app.loop.stats;
	const struct softfab_stats *s = &fab.stats;
	say(LOG_INFO,
	    "entity %016llx mac %012llx | link %s gm %016llx asCapable %d | loop passes %u events %u rx %u bad %u "
	    "| fabric rx %llu admitted %llu tx %llu err %llu lost %llu filter_mismatch %u",
	    (unsigned long long)econf.entity.entity_id, (unsigned long long)econf.entity.mac,
	    fab.link_up ? "up" : "down", (unsigned long long)fab.gm_id, fab.as_capable, l->passes, l->events,
	    l->rx_records, l->rx_bad, (unsigned long long)s->rx_frames, (unsigned long long)s->rx_admitted,
	    (unsigned long long)s->tx_frames, (unsigned long long)s->tx_errors, (unsigned long long)s->tx_lost,
	    fab.model.filter_mismatch);
	say(LOG_INFO, "MAAP %s base %012llx count %u", dp->maap_valid ? "valid" : "none",
	    (unsigned long long)dp->maap_base, dp->maap_count);
	for (unsigned k = 0; k < acmp_cfg.n_sinks; ++k) {
		const struct acmp_sink *x = &app.acmp.acmp.sinks[k];
		say(LOG_INFO, "sink %u: state %d bound %d talker %016llx/%u started %d status %u", k, (int)x->state,
		    x->bound, (unsigned long long)x->binding.talker_entity_id, x->binding.talker_unique_id, x->started,
		    x->acmp_status);
	}
}

static void on_signal(int sig)
{
	if (sig == SIGUSR1) {
		dump_requested = 1;
	} else {
		stop_requested = 1;
	}
}

// ---- main ----------------------------------------------------------------------

static void usage(FILE *to)
{
	fprintf(to,
		"usage: milan-ctrld [options]\n"
		"  -i IFACE   AVB interface (eth0)\n"
		"  -e FILE    entity description (/etc/milan/entity.conf)\n"
		"  -p PATH    ptp4l read-only management socket (/var/run/ptp4lro), \"none\" for no gPTP\n"
		"  -l PATH    our socket for ptp4l's replies (/var/run/milan-ctrld.ptp)\n"
		"  -d NAME    datapath block, a shm_open() name (" MILAN_DP_NAME ")\n"
		"  -V VID     VLAN of the talker's streams (2)\n"
		"  -n         no SRP domain (a direct link, no MSRP bridge): a settled sink's talker counts as registered\n"
		"  -s         log to syslog\n"
		"  -v         more logging (twice: the firmware's own prints)\n"
		"SIGUSR1 logs the state; SIGTERM or SIGINT departs (ENTITY_DEPARTING) and exits.\n");
}

static int parse(int argc, char **argv)
{
	int c;
	while ((c = getopt(argc, argv, "i:e:p:l:d:V:nsvh")) != -1) {
		switch (c) {
		case 'i': opt.ifname = optarg; break;
		case 'e': opt.entity_path = optarg; break;
		case 'p': opt.ptp_server = strcmp(optarg, "none") == 0 ? NULL : optarg; break;
		case 'l': opt.ptp_local = optarg; break;
		case 'd': opt.dp_name = optarg; break;
		case 'V': opt.vlan = (uint16_t)strtoul(optarg, NULL, 0); break;
		case 's': opt.use_syslog = true; break;
		case 'n': opt.no_srp = true; break;
		case 'v': opt.verbose++; break;
		case 'h': usage(stdout); exit(0);
		default: usage(stderr); return -1;
		}
	}
	return optind == argc ? 0 : -1;
}

static int compose(void)
{
	const struct adp_entity *e = &econf.entity;
	if (e->talker_stream_sources > MILAN_DP_MAX_SOURCES || e->listener_stream_sinks > MILAN_DP_MAX_SINKS ||
	    e->talker_stream_sources > ACMP_MAX_SOURCES || e->listener_stream_sinks > ACMP_MAX_SINKS) {
		say(LOG_ERR, "entity: %u sources and %u sinks exceed this build", e->talker_stream_sources,
		    e->listener_stream_sinks);
		return -1;
	}
	acmp_cfg.entity_id = e->entity_id;
	acmp_cfg.n_interfaces = 1u;
	acmp_cfg.mac[0] = e->mac;
	acmp_cfg.n_sinks = e->listener_stream_sinks;
	acmp_cfg.n_sources = e->talker_stream_sources;

	milan_dp_begin(dp);
	dp->entity_id = e->entity_id;
	dp->mac = e->mac;
	dp->n_sources = e->talker_stream_sources;
	dp->n_sinks = e->listener_stream_sinks;
	for (unsigned i = 0; i < dp->n_sources; ++i) {
		// Milan v1.2 / IEEE 1722.1-2021: the talker's MAC, then its unique ID
		dp->sources[i].stream_id = (e->mac << 16) | i;
		dp->sources[i].vlan_id = opt.vlan;
	}
	milan_dp_end(dp);

	size_t need = ctrl_pool_arena_bytes(pool_classes, 1u);
	if (need > sizeof pool_arena) {
		say(LOG_ERR, "the pool needs %zu arena bytes, this build has %zu", need, sizeof pool_arena);
		return -1;
	}
	struct ctrl_app_config cfg = {
		.entity = e,
		.arena = pool_arena,
		.arena_bytes = sizeof pool_arena,
		.classes = pool_classes,
		.n_classes = 1u,
		.sink = debug_sink,
		.acmp = acmp_cfg.n_sinks + acmp_cfg.n_sources > 0u ? &acmp_cfg : NULL,
		.acmp_env = &acmp_env,
		.maap_allocation = e->talker_stream_sources > 0u ? maap_allocated : NULL,
	};
	if (!ctrl_app_compose(&app, &cfg)) {
		say(LOG_ERR, "the firmware refused its composition");
		return -1;
	}
	if (!ctrl_app_open(&app, &cfg)) {
		say(LOG_ERR, "the soft fabric does not carry this firmware's contract");
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (parse(argc, argv) != 0) {
		return 2;
	}
	if (opt.use_syslog) {
		openlog("milan-ctrld", LOG_PID, LOG_DAEMON);
	}
	if (entity_conf_load(&econf, opt.entity_path) != 0) {
		return 1;
	}
	struct softfab_config fcfg = {
		.ifname = opt.ifname,
		.ptp_server = opt.ptp_server,
		.ptp_local = opt.ptp_local,
		.ptp_transport_specific = 1u,
		.ptp_poll_ms = opt.ptp_poll_ms,
	};
	if (softfab_open(&fab, &fcfg) != 0) {
		return 1;
	}
	entity_conf_finish(&econf, fab.mac);
	dp = milan_dp_create(opt.dp_name);
	if (dp == NULL) {
		say(LOG_ERR, "datapath block %s: %s", opt.dp_name, strerror(errno));
		return 1;
	}
	if (compose() != 0) {
		return 1;
	}
	say(LOG_INFO, "entity %016llx on %s (mac %012llx, model %016llx, %u sources, %u sinks%s)",
	    (unsigned long long)econf.entity.entity_id, opt.ifname, (unsigned long long)econf.entity.mac,
	    (unsigned long long)econf.entity.entity_model_id, econf.entity.talker_stream_sources,
	    econf.entity.listener_stream_sinks, opt.no_srp ? ", no SRP domain" : "");

	struct sigaction sa = {.sa_handler = on_signal};
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);

	while (!stop_requested) {
		unsigned handled = ctrl_loop_service(&app.loop);
		for (unsigned k = 0; k < acmp_cfg.n_sinks && k < ACMP_MAX_SINKS; ++k) {
			if (tk_owed[k]) {
				tk_owed[k] = false;
				acmp_tk_registered(&app.acmp.acmp, k, false);
				handled++;
			}
		}
		softfab_pump(&fab, handled == 0u);
		publish_gptp();
		if (dump_requested) {
			dump_requested = 0;
			dump();
		}
	}

	// shut ADP down and serve the loop long enough for ENTITY_DEPARTING
	adp_mbx_set_enable(&app.adp, false);
	uint64_t until = softfab_now_ms(&fab) + SHUTDOWN_MS;
	while (softfab_now_ms(&fab) < until) {
		unsigned handled = ctrl_loop_service(&app.loop);
		softfab_pump(&fab, false);
		if (handled == 0u) {
			usleep(1000);
		}
	}
	dump();
	softfab_close(&fab);
	say(LOG_INFO, "departed");
	return 0;
}
