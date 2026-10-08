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
//   acmp_env.persist              the saved-state store marks the binding
//                                 record (nvm_store_changed), as on the RISC-V
//                                 platform; the store is milan-fpga's KLJ2
//                                 journal on a file (nvm_file.h, -N)
//   acmp_env.changed              logged (AECP's notifications arrive with it)
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

#include "acmp_nvm.h"
#include "ctrl_app.h"
#include "datapath.h"
#include "entity_conf.h"
#include "nvm_file.h"
#include "nvm_klj2.h"
#include "nvm_store.h"
#include "softfab.h"

#define DEFAULT_VLAN 2u                 // Milan v1.2 4.2.7: the SR class default VID
#define SHUTDOWN_MS 200u                // service time left for ENTITY_DEPARTING

static struct {
	const char *ifname;
	const char *entity_path;
	const char *gptp_shm;           // flexptpd's status block, NULL for none
	const char *dp_name;
	const char *journal;            // the saved-state journal file, NULL for none
	uint16_t vlan;
	unsigned gptp_poll_ms;
	bool use_syslog;
	bool no_srp;
	int verbose;
} opt = {
	.ifname = "eth0",
	.entity_path = "/etc/milan/entity.conf",
	.gptp_shm = NULL,               // "flexptpd.<interface>" unless -g says otherwise
	.dp_name = MILAN_DP_NAME,
	.journal = "/var/lib/milan/journal.bin",
	.vlan = DEFAULT_VLAN,
	.gptp_poll_ms = 100u,
};

static bool gptp_none;                  // -g none: no gPTP plane
static char gptp_default[64];

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
static struct acmp_nvm binding_owner;
static struct nvm_flash journal_port;

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
	if (opt.journal != NULL) {
		nvm_store_changed(NVM_G_BIND, sink);
	}
	say(LOG_DEBUG, "sink %u: binding changed", sink);
}

// Every saved group but the bindings belongs to AECP's owners (#14); until
// they exist, those records are taken and left as staged.
//
// The model. On the RISC-V end station it is proven by the AEM image's CRC at
// boot; without it the restore ends CLOSED and no writer runs. The PB2 has no
// AEM image yet (#9), so the model counts as proven when the entity advertises
// the model ID the store's shape was derived for (config/nvm_shape_gen.h): the
// records are then judged against the shape they were written in.
static int others_model_ready(void *ctx)
{
	(void)ctx;
	return econf.entity.entity_model_id ==
	       (((uint64_t)MILAN_NVM_SHAPE_MODEL_ID_HI << 32) | MILAN_NVM_SHAPE_MODEL_ID_LO);
}

static enum nvm_apply others_apply(void *ctx, unsigned int group, unsigned int index, const uint8_t *payload,
				   unsigned int len)
{
	(void)ctx;
	(void)group;
	(void)index;
	(void)payload;
	(void)len;
	return NVM_APPLIED;
}

static enum nvm_apply others_settle(void *ctx)
{
	(void)ctx;
	return NVM_APPLIED;
}

static int others_rollback(void *ctx, enum nvm_walk walk)
{
	(void)ctx;
	(void)walk;
	return 0;
}

static int others_latch(void *ctx, unsigned int group, unsigned int index, uint8_t *payload, unsigned int len)
{
	(void)ctx;
	(void)group;
	(void)index;
	(void)payload;
	(void)len;
	return 0;
}

static void others_release(void *ctx)
{
	(void)ctx;
}

static const struct nvm_state others = {
	others_model_ready, others_apply, others_settle, others_rollback, others_latch, others_release, NULL,
};

static void report_store(const char *when)
{
	if (opt.journal == NULL) {
		return;
	}
	const struct nvm_status *s = nvm_store_status();
	say(LOG_INFO,
	    "saved state %s: slot %s seq %u (A %u, B %u) | bindings %s, rest %s | phase %d dirty %d pending %d "
	    "| commits %u ok, %u failed, %u skipped | unread 0x%x read faults %u",
	    when, s->auth == 0 ? "A" : s->auth == 1 ? "B" : "none", s->seq, s->seq_a, s->seq_b,
	    s->bind_terminal == NVM_T_COMPLETE ? "restored" : s->bind_terminal == NVM_T_BLANK ? "blank" : "other",
	    s->terminal == NVM_T_CLOSED ? "closed" : s->terminal == NVM_T_COMPLETE ? "restored" : "other", (int)s->phase,
	    s->dirty, s->pending, s->commits_ok, s->commits_failed, s->commits_skipped, s->unread, s->read_faults);
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
	report_store("now");
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
		"  -g NAME    flexptpd's gPTP status block, a shm_open() name (flexptpd.IFACE), \"none\" for no gPTP\n"
		"  -d NAME    datapath block, a shm_open() name (" MILAN_DP_NAME ")\n"
		"  -N FILE    saved-state journal (/var/lib/milan/journal.bin), \"none\" for none\n"
		"  -V VID     VLAN of the talker's streams (2)\n"
		"  -n         no SRP domain (a direct link, no MSRP bridge): a settled sink's talker counts as registered\n"
		"  -s         log to syslog\n"
		"  -v         more logging (twice: the firmware's own prints)\n"
		"SIGUSR1 logs the state; SIGTERM or SIGINT departs (ENTITY_DEPARTING) and exits.\n");
}

static int parse(int argc, char **argv)
{
	int c;
	while ((c = getopt(argc, argv, "i:e:g:d:N:V:nsvh")) != -1) {
		switch (c) {
		case 'i': opt.ifname = optarg; break;
		case 'e': opt.entity_path = optarg; break;
		case 'g':
			gptp_none = strcmp(optarg, "none") == 0;
			opt.gptp_shm = gptp_none ? NULL : optarg;
			break;
		case 'd': opt.dp_name = optarg; break;
		case 'N': opt.journal = strcmp(optarg, "none") == 0 ? NULL : optarg; break;
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
	// the RISC-V platform's boot order: the store restores the bindings between
	// the composition and the open, so they are in place before a channel opens
	if (opt.journal != NULL) {
		if (cfg.acmp == NULL) {
			say(LOG_ERR, "a saved state needs ACMP: the entity has no stream");
			return -1;
		}
		milan_nvm_entity_id_lo = (uint32_t)e->entity_id;
		milan_nvm_entity_id_hi = (uint32_t)(e->entity_id >> 32);
		milan_nvm_model_id_lo = (uint32_t)e->entity_model_id;
		milan_nvm_model_id_hi = (uint32_t)(e->entity_model_id >> 32);
		int rc = nvm_file_open(opt.journal, &journal_port);
		if (rc != 0) {
			say(LOG_ERR, "saved state %s: %s", opt.journal, strerror(-rc));
			return -1;
		}
		acmp_nvm_init(&binding_owner, &app.acmp.acmp, NVM_G_BIND, &others);
		nvm_store_boot(&journal_port, &binding_owner.port);
		if (!ctrl_loop_add_tick(&app.loop, nvm_store_service)) {
			say(LOG_ERR, "no room for the store's tick");
			return -1;
		}
		report_store("at boot");
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
	// flexptpd's block is named after the interface it runs on
	if (opt.gptp_shm == NULL && !gptp_none) {
		snprintf(gptp_default, sizeof gptp_default, "flexptpd.%s", opt.ifname);
		opt.gptp_shm = gptp_default;
	}

	struct softfab_config fcfg = {
		.ifname = opt.ifname,
		.gptp_shm = opt.gptp_shm,
		.gptp_poll_ms = opt.gptp_poll_ms,
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
	// a change still inside the store's debounce is written before the exit
	if (opt.journal != NULL) {
		const struct nvm_status *s = nvm_store_status();
		if (s->dirty || s->pending) {
			nvm_store_commit_now();
			until = softfab_now_ms(&fab) + 3000u;
			while (softfab_now_ms(&fab) < until && (s->dirty || s->pending || s->phase != NVM_P_IDLE)) {
				ctrl_loop_service(&app.loop);
				softfab_pump(&fab, false);
				usleep(1000);
			}
		}
		nvm_file_close();
	}
	dump();
	softfab_close(&fab);
	say(LOG_INFO, "departed");
	return 0;
}
