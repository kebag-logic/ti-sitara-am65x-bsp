// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// milan_ctrld.c - the Milan control plane of the PocketBeagle 2 USB-to-Milan
// bridge: milan-fpga's Mark II control-plane firmware, unchanged, on the soft
// fabric (softfab.h), with the integrator's ports wired to Linux.
//
// THE BOOT is the split platform's (sw/firmware/ctrl/plat/ctrl_plat.c,
// ctrl_plat_boot), in its order, with the steps that program fabric CSRs left
// to the soft fabric:
//   4    the generated AEM image (config/aecp_entity_gen.h) is verified
//        against the CRC its generator emitted (aecp_image_load). Its
//        identity is the config's; this board's is eth0's MAC, so a copy of
//        the verified image takes eth0's in the three mac-derived fields
//        (ENTITY entity_id, AVB_INTERFACE mac_address and clock_identity) and
//        is loaded against its own CRC: that copy is the host AEM store;
//   5    the application (ADP, ACMP, MAAP), AECP and contract 2.3's face are
//        composed (ctrl_app_compose, ctrl_app_compose_aecp,
//        ctrl_app_aecp_attach_face). The face's windows and commits are
//        answered by the datapath behind it here (aecp_far.h);
//   6-7  the saved state is read and applied through AECP's owner
//        (aecp_nvm_init, acmp_nvm_init on its port, nvm_store_boot), the
//        store's tick and step joining the loop;
//   8    the mailbox opens with ADP held (ctrl_app_open_held), and AECP is
//        released at the store's accepted terminal with the face
//        acknowledged (ctrl_app_open_aecp);
//   9    ADP starts last, once AECP and ACMP accept requests
//        (ctrl_app_release_adp; Milan v1.2 5.6.1).
// Two of the platform's parts have no counterpart: SRP is not attached (the
// bridge has no SRP yet, #14), and the receive monitor's per-input format
// contexts (#946) are fabric registers for AAF inputs above 0, of which this
// entity has none. A stage that refuses ends the program, as the platform's
// leaves its entity disabled.
//
// The integrator's ports land here (the AECP bridge wraps ACMP's, so the lock
// is AECP's and the bindings are persisted through the store's state port):
//
//   port                          here
//   ----------------------------  --------------------------------------------
//   acmp_env.locked               never locked here: the bridge answers with
//                                 AECP's lock
//   acmp_env.source               the talker's stream: the entity MAC and the
//                                 source index as stream_id, MAAP's address
//   acmp_env.srp                  the listener's settled stream, published for
//                                 the media plane; with -n (no SRP domain: a
//                                 direct link, no MSRP bridge), its talker also
//                                 counts as registered, so the sink holds
//                                 SETTLED_RSV_OK instead of re-probing every
//                                 TMR_NO_TK
//   acmp_env.persist, .changed    logged: the bridge persists the binding and
//                                 notifies AECP's controllers
//   maap_allocation               the MAAP range, published for the media
//                                 plane and read back by acmp_env.source
//   aecp_ports.random             the loop's millisecond clock, the platform's
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
#include "aecp_entity_gen.h"
#include "aecp_far.h"
#include "aecp_image.h"
#include "ctrl_app_aecp.h"
#include "datapath.h"
#include "entity_conf.h"
#include "media.h"
#include "nvm_file.h"
#include "nvm_store.h"
#include "softfab.h"
#include "wire.h"

#define DEFAULT_VLAN 2u                 // Milan v1.2 4.2.7: the SR class default VID
#define CLASS_A_PCP 3u                  // the talker's PCP, SR class A (avb.env AVB_CLASS_A_PRIO)
#define SHUTDOWN_MS 200u                // service time left for ENTITY_DEPARTING

static struct {
	const char *ifname;
	const char *entity_path;
	const char *gptp_shm;           // flexptpd's status block, NULL for none
	const char *dp_name;
	const char *media_name;         // milan-mediad's block, for AECP's counters
	const char *journal;            // the saved-state journal file, NULL for one in memory
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
	.media_name = MILAN_MEDIA_NAME,
	.journal = "/data/milan/journal.bin",
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
static struct ctrl_app_config app_cfg;
static struct entity_conf econf;
static struct acmp_config acmp_cfg;
static struct milan_dp *dp;
static struct nvm_flash journal_port;
static struct aecp_far far;

// The application's pool, carved by the composition for lwSRP's port. SRP is
// not attached here (#14), so its entity-sized classes (srp_mbx.c) are not
// linked; the arena also holds the alignment and a flag per block
// (ctrl_pool_arena_bytes), so it is sized with room and checked at start.
static const struct ctrl_pool_class pool_classes[] = {{32u, 8u}};
static max_align_t pool_arena[1024u / sizeof(max_align_t)];

// ---- the host AEM store and AECP's state: the platform's storage, sized by the generated image ----

static struct aecp_model model;
static struct aecp_descriptor descriptors[AECP_ENTITY_DESCRIPTORS];
static uint8_t values[AECP_ENTITY_VALUE_BYTES];
static struct aecp_event events[AECP_ENTITY_DESCRIPTORS];
static uint32_t latency[AECP_ENTITY_OUTPUTS];
#if AECP_MBX_OWED_SLOTS != 0u
static struct aecp_owed owed[AECP_MBX_OWED_SLOTS];
#endif
static struct aecp_mapping map_rows[AECP_ENTITY_MAP_ROWS];
static struct aecp_mapping map_scratch[AECP_ENTITY_MAP_MAX];
static struct aecp_map maps[AECP_ENTITY_MAPS] = AECP_ENTITY_MAPS_INIT(map_rows);
static const struct aecp_name_run names[AECP_ENTITY_NAME_RUNS] = AECP_ENTITY_NAME_RUNS_INIT;
// the verified image with this board's identity: the descriptors' defaults point into it
static uint8_t board_image[sizeof aecp_entity_image];

static struct aecp_mbx aecp;
static struct aecp_config aecp_cfg;
static struct aecp_ports aecp_platform;
static struct aecp_face face;
static struct ctrl_app_aecp bridge;
static struct aecp_nvm state;
static struct acmp_nvm binding;

_Static_assert(CTRL_SRP_SOURCES <= ACMP_MAX_SOURCES && CTRL_SRP_SINKS <= ACMP_MAX_SINKS,
	       "the generated stream counts fit ACMP");
_Static_assert(CTRL_SRP_SOURCES <= MILAN_DP_MAX_SOURCES && CTRL_SRP_SINKS <= MILAN_DP_MAX_SINKS,
	       "the generated stream counts fit the datapath block");
_Static_assert(CTRL_AEM_STREAM_INPUTS <= AECP_FAR_STREAMS && CTRL_AEM_STREAM_OUTPUTS <= AECP_FAR_STREAMS,
	       "the generated streams fit the far side's format table");

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

// AECP's bridge answers the lock (ctrl_app_aecp.c); the platform holds none of its own
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

// the bridge marks the binding record for the store (ctrl_app_aecp.c persist, deliver)
static void env_persist(void *ctx, unsigned sink)
{
	(void)ctx;
	say(LOG_DEBUG, "sink %u: binding changed", sink);
}

// ACMP's bound state into the datapath block, and the count of not-bound to
// bound changes that zeroes an input's Milan counters (Milan v1.2 5.3.8.10).
// Read every loop pass, so a binding the saved state restores at boot counts
// as one too.
static void publish_bound(void)
{
	for (unsigned k = 0; k < acmp_cfg.n_sinks && k < ACMP_MAX_SINKS && k < MILAN_DP_MAX_SINKS; ++k) {
		bool bound = app.acmp.acmp.sinks[k].bound;
		if (bound != (dp->sinks[k].bound != 0u)) {
			milan_dp_begin(dp);
			dp->sinks[k].bound = bound;
			dp->sinks[k].binds += bound ? 1u : 0u;
			milan_dp_end(dp);
		}
	}
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

// the platform's random port: the loop's millisecond clock, as on the split target
static uint32_t random_value(void *ctx)
{
	(void)ctx;
	return mbx_now_ms();
}

// ---- status ------------------------------------------------------------------

static void report_store(const char *when)
{
	const struct nvm_status *s = nvm_store_status();
	say(LOG_INFO,
	    "saved state %s (%s): slot %s seq %u (A %u, B %u) | bindings %s, rest %s | phase %d dirty %d pending %d "
	    "| commits %u ok, %u failed, %u skipped | unread 0x%x read faults %u legacy 0x%x",
	    when, opt.journal != NULL ? opt.journal : "in memory", s->auth == 0 ? "A" : s->auth == 1 ? "B" : "none",
	    s->seq, s->seq_a, s->seq_b,
	    s->bind_terminal == NVM_T_COMPLETE ? "restored" : s->bind_terminal == NVM_T_BLANK ? "blank" : "other",
	    s->terminal == NVM_T_CLOSED ? "closed" : s->terminal == NVM_T_COMPLETE ? "restored"
	    : s->terminal == NVM_T_BLANK ? "blank" : "other", (int)s->phase,
	    s->dirty, s->pending, s->commits_ok, s->commits_failed, s->commits_skipped, s->unread, s->read_faults,
	    s->legacy);
}

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
	uint64_t owner = 0;
	bool locked = aecp_locked(&aecp.core, &owner);
	say(LOG_INFO, "AECP %s, %s %016llx | face commits %u late %u unanswered %u | far answers %u silent %u "
	    "commits %u edits refused %u | media block %s",
	    aecp.core.open ? "open" : "closed", locked ? "locked by" : "unlocked", (unsigned long long)owner,
	    face.commits, face.late, face.unanswered, far.answers, far.silent, far.commits, far.refused_edits,
	    far.media_valid ? "read" : "absent");
	say(LOG_INFO, "AECP core: ready %d response_owed %d notify %d owed %u waiting %d refusing %d | busy drops %u "
	    "malformed %u ignored %u", aecp_ready(&aecp.core), aecp.core.response_owed, aecp.core.notify,
	    aecp.core.owed_count, aecp.core.waiting, aecp.core.refusing, aecp.core.busy_drops, aecp.core.malformed,
	    aecp.core.ignored);
	say(LOG_INFO, "ingress rate drops: adp %u acmp %u aecp %u maap %u | rx drops: adp %u acmp %u aecp %u maap %u",
	    fab.model.ch[MBX_CH_ADP].rate_drop, fab.model.ch[MBX_CH_ACMP].rate_drop, fab.model.ch[MBX_CH_AECP].rate_drop,
	    fab.model.ch[MBX_CH_MAAP].rate_drop, fab.model.ch[MBX_CH_ADP].rx_drop, fab.model.ch[MBX_CH_ACMP].rx_drop,
	    fab.model.ch[MBX_CH_AECP].rx_drop, fab.model.ch[MBX_CH_MAAP].rx_drop);
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

// ---- step 4: the host AEM store ------------------------------------------------

// IEEE 1722.1-2021 Table 7-1, and where the image carries the station identity:
// ENTITY entity_id and entity_model_id (Table 7-2), AVB_INTERFACE mac_address and
// clock_identity (Table 7-13). Each is a field the builder bakes from the
// config's MAC and the mac-derived entity_id (gen_aemi_image.py).
#define DESC_ENTITY 0x0000u
#define DESC_AVB_INTERFACE 0x0009u
#define ENTITY_ID_AT 4u
#define ENTITY_MODEL_ID_AT 12u
#define ENTITY_CAPS_AT 20u
#define ENTITY_SOURCES_AT 24u
#define ENTITY_TALKER_CAPS_AT 26u
#define ENTITY_SINKS_AT 28u
#define ENTITY_LISTENER_CAPS_AT 30u
#define AVB_MAC_AT 70u
#define AVB_CLOCK_ID_AT 78u
#define STREAM_FORMAT_AT 74u            // a STREAM_INPUT's or STREAM_OUTPUT's current_format (Table 7-8)

// CRC-32 (ISO-HDLC, the image manifest's), over the board's copy
static uint32_t crc32(const uint8_t *data, size_t bytes)
{
	uint32_t crc = UINT32_MAX;
	for (size_t n = 0; n < bytes; ++n) {
		crc ^= data[n];
		for (unsigned bit = 0; bit < 8u; ++bit) {
			crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xedb88320u);
		}
	}
	return ~crc;
}

static const struct aecp_descriptor *find(uint16_t type, uint16_t index)
{
	for (size_t n = 0; n < model.count; ++n) {
		const struct aecp_descriptor *d = &model.descriptors[n];
		if (d->configuration == 0u && d->type == type && d->index == index) {
			return d;
		}
	}
	return NULL;
}

// ADP's fields (entity.conf) are the image's ENTITY's: IEEE 1722.1-2021 Table 7-2
// makes each "the same as" ADP's, and both are generated from one config
static bool image_agrees(const struct aecp_descriptor *e)
{
	const struct adp_entity *a = &econf.entity;
	const uint8_t *v = e->defaults;
	if (wire_be64(v + ENTITY_MODEL_ID_AT) != a->entity_model_id ||
	    wire_be32(v + ENTITY_CAPS_AT) != a->entity_capabilities ||
	    wire_be16(v + ENTITY_SOURCES_AT) != a->talker_stream_sources ||
	    wire_be16(v + ENTITY_TALKER_CAPS_AT) != a->talker_capabilities ||
	    wire_be16(v + ENTITY_SINKS_AT) != a->listener_stream_sinks ||
	    wire_be16(v + ENTITY_LISTENER_CAPS_AT) != a->listener_capabilities) {
		say(LOG_ERR, "%s does not describe the compiled AEM image (model %016llx, %u sources, %u sinks): "
		    "regenerate both (tools/gen-config.sh)", opt.entity_path,
		    (unsigned long long)wire_be64(v + ENTITY_MODEL_ID_AT), (unsigned)wire_be16(v + ENTITY_SOURCES_AT),
		    (unsigned)wire_be16(v + ENTITY_SINKS_AT));
		return false;
	}
	if (a->talker_stream_sources != CTRL_SRP_SOURCES || a->listener_stream_sinks != CTRL_SRP_SINKS) {
		say(LOG_ERR, "%s: %u sources and %u sinks, the firmware is compiled for %u and %u", opt.entity_path,
		    a->talker_stream_sources, a->listener_stream_sinks, CTRL_SRP_SOURCES, CTRL_SRP_SINKS);
		return false;
	}
	return true;
}

static bool load_image(void)
{
	// the generated image, as aecp_entity.py emitted it, against its generated CRC
	if (!aecp_image_load(&model, aecp_entity_image, sizeof aecp_entity_image, AECP_ENTITY_CRC, descriptors,
			     AECP_ENTITY_DESCRIPTORS, values, sizeof values)) {
		say(LOG_ERR, "the generated AEM image fails its CRC or form");
		return false;
	}
	const struct aecp_descriptor *e = find(DESC_ENTITY, 0), *avb = find(DESC_AVB_INTERFACE, 0);
	if (e == NULL || avb == NULL || e->length < ENTITY_LISTENER_CAPS_AT + 2u ||
	    avb->length < AVB_CLOCK_ID_AT + 8u || !image_agrees(e)) {
		say(LOG_ERR, "the generated AEM image has no ENTITY or AVB_INTERFACE 0 of the expected form");
		return false;
	}
	size_t ent = (size_t)(e->defaults - aecp_entity_image), mac = (size_t)(avb->defaults - aecp_entity_image);
	// the generator baked the config's identity into exactly these three fields
	uint64_t baked_eid = wire_be64(e->defaults + ENTITY_ID_AT);
	uint64_t baked_mac = (uint64_t)wire_be32(avb->defaults + AVB_MAC_AT) << 16 | wire_be16(avb->defaults + AVB_MAC_AT + 4u);
	uint64_t baked_clock = wire_be64(avb->defaults + AVB_CLOCK_ID_AT);
	uint8_t m[6];
	wire_put_be(m, baked_mac, 6);
	if (baked_eid != entity_id_from_mac(m) || baked_clock != baked_eid) {
		say(LOG_ERR, "the generated AEM image's identity fields are not one mac-derived identity");
		return false;
	}
	memcpy(board_image, aecp_entity_image, sizeof board_image);
	wire_put_be(board_image + ent + ENTITY_ID_AT, econf.entity.entity_id, 8);
	wire_put_be(board_image + mac + AVB_MAC_AT, econf.entity.mac, 6);
	wire_put_be(board_image + mac + AVB_CLOCK_ID_AT, entity_id_from_mac(fab.mac), 8);
	if (!aecp_image_load(&model, board_image, sizeof board_image, crc32(board_image, sizeof board_image),
			     descriptors, AECP_ENTITY_DESCRIPTORS, values, sizeof values)) {
		say(LOG_ERR, "the board's AEM image refused");
		return false;
	}
	say(LOG_INFO, "AEM image: %zu bytes, %zu descriptors, generated CRC %08x verified; this board's identity "
	    "%016llx, CRC %08x", sizeof board_image, model.count, (unsigned)AECP_ENTITY_CRC,
	    (unsigned long long)econf.entity.entity_id, (unsigned)crc32(board_image, sizeof board_image));
	return true;
}

// ---- step 5: the composition ----------------------------------------------------

static void far_formats(struct aecp_far_config *fc)
{
	for (unsigned k = 0; k < CTRL_AEM_STREAM_INPUTS; ++k) {
		const struct aecp_descriptor *d = find(5u, (uint16_t)k);
		fc->input_format[k] = d != NULL && d->length >= STREAM_FORMAT_AT + 8u ? wire_be64(d->defaults + STREAM_FORMAT_AT) : 0u;
	}
	for (unsigned k = 0; k < CTRL_AEM_STREAM_OUTPUTS; ++k) {
		const struct aecp_descriptor *d = find(6u, (uint16_t)k);
		fc->output_format[k] = d != NULL && d->length >= STREAM_FORMAT_AT + 8u ? wire_be64(d->defaults + STREAM_FORMAT_AT) : 0u;
	}
	fc->n_inputs = CTRL_AEM_STREAM_INPUTS;
	fc->n_outputs = CTRL_AEM_STREAM_OUTPUTS;
}

static bool compose(void)
{
	const struct adp_entity *e = &econf.entity;
	acmp_cfg = (struct acmp_config){.entity_id = e->entity_id, .n_interfaces = MBX_N_IF,
		.n_sinks = CTRL_SRP_SINKS, .n_sources = CTRL_SRP_SOURCES};
	acmp_cfg.mac[0] = e->mac;

	milan_dp_begin(dp);
	dp->entity_id = e->entity_id;
	dp->mac = e->mac;
	dp->n_sources = CTRL_SRP_SOURCES;
	dp->n_sinks = CTRL_SRP_SINKS;
	for (unsigned i = 0; i < dp->n_sources; ++i) {
		// IEEE 1722.1-2021 7.2.3: the talker's MAC, then the STREAM_OUTPUT's unique ID
		dp->sources[i].stream_id = (e->mac << 16) | i;
		dp->sources[i].vlan_id = opt.vlan;
	}
	milan_dp_end(dp);

	size_t need = ctrl_pool_arena_bytes(pool_classes, 1u);
	if (need > sizeof pool_arena) {
		say(LOG_ERR, "the pool needs %zu arena bytes, this build has %zu", need, sizeof pool_arena);
		return false;
	}
	app_cfg = (struct ctrl_app_config){
		.entity = e, .arena = pool_arena, .arena_bytes = sizeof pool_arena, .classes = pool_classes,
		.n_classes = 1u, .sink = debug_sink, .acmp = &acmp_cfg, .acmp_env = &acmp_env,
		.maap_allocation = maap_allocated,
	};
	aecp_cfg = (struct aecp_config){.entity_id = e->entity_id, .interfaces = MBX_N_IF, .model = &model,
		.maps = maps, .map_count = AECP_ENTITY_MAPS, .events = events, .latency = latency, .reboot = false};
	aecp_cfg.mac[0] = e->mac;
#if AECP_MBX_OWED_SLOTS != 0u
	aecp_cfg.owed = owed;
	aecp_cfg.owed_slots = AECP_MBX_OWED_SLOTS;
	aecp_cfg.queue = &aecp_owed_queue;
#endif
	aecp_platform = (struct aecp_ports){.random = random_value};
	aecp_face_init(&face, &aecp_platform);
	if (!ctrl_app_compose(&app, &app_cfg) ||
	    !ctrl_app_compose_aecp(&app, &bridge, &aecp, &aecp_cfg, &face.ports, &state) ||
	    !ctrl_app_aecp_attach_face(&bridge, &face)) {
		say(LOG_ERR, "the firmware refused its composition");
		return false;
	}
	struct aecp_far_config fc = {.media_name = opt.media_name, .vlan = opt.vlan, .class_a_priority = CLASS_A_PCP};
	far_formats(&fc);
	aecp_far_init(&far, &fab, dp, &fc);
	return true;
}

// ---- steps 6 to 9 ---------------------------------------------------------------

static bool boot(void)
{
	if (!load_image() || !compose()) {
		return false;
	}
	// steps 6-7: the saved state, bindings first, through AECP's owner, before anything opens
	const struct adp_entity *e = &econf.entity;
	milan_nvm_entity_id_lo = (uint32_t)e->entity_id;
	milan_nvm_entity_id_hi = (uint32_t)(e->entity_id >> 32);
	milan_nvm_model_id_lo = (uint32_t)e->entity_model_id;
	milan_nvm_model_id_hi = (uint32_t)(e->entity_model_id >> 32);
	if (e->entity_model_id != (((uint64_t)MILAN_NVM_SHAPE_MODEL_ID_HI << 32) | MILAN_NVM_SHAPE_MODEL_ID_LO)) {
		say(LOG_ERR, "the store's shape (config/nvm_shape_gen.h) is another model's: regenerate it");
		return false;
	}
	int rc = nvm_file_open(opt.journal, &journal_port);
	if (rc != 0) {
		say(LOG_ERR, "saved state %s: %s", opt.journal, strerror(-rc));
		return false;
	}
	aecp_nvm_init(&state, &aecp.core, names, AECP_ENTITY_NAME_RUNS, map_scratch, AECP_ENTITY_MAP_MAX);
	acmp_nvm_init(&binding, &app.acmp.acmp, NVM_G_BIND, &state.port);
	nvm_store_boot(&journal_port, &binding.port);
	if (!ctrl_loop_add_tick(&app.loop, nvm_store_tick) ||
	    !ctrl_loop_set_step(&app.loop, nvm_store_step, NULL, nvm_store_clock, NULL)) {
		say(LOG_ERR, "no room for the store's tick and step");
		return false;
	}
	report_store("at boot");
	// step 8: the mailbox with ADP held, then AECP at the accepted terminal, its face acknowledged
	if (!ctrl_app_open_held(&app, &app_cfg)) {
		say(LOG_ERR, "the soft fabric does not carry this firmware's contract");
		return false;
	}
	if (!ctrl_app_open_aecp(&bridge)) {
		say(LOG_ERR, "AECP not released: the saved state's terminal was not accepted, or the face not acknowledged");
		return false;
	}
	// step 9: ADP last, once AECP and ACMP accept requests (Milan v1.2 5.6.1)
	ctrl_app_release_adp(&app);
	return true;
}

// ---- main ----------------------------------------------------------------------

static void usage(FILE *to)
{
	fprintf(to,
		"usage: milan-ctrld [options]\n"
		"  -i IFACE   AVB interface (eth0)\n"
		"  -e FILE    entity description (/etc/milan/entity.conf), the compiled AEM image's\n"
		"  -g NAME    flexptpd's gPTP status block, a shm_open() name (flexptpd.IFACE), \"none\" for no gPTP\n"
		"  -d NAME    datapath block, a shm_open() name (" MILAN_DP_NAME ")\n"
		"  -m NAME    milan-mediad's media block, for AECP's stream counters (" MILAN_MEDIA_NAME ")\n"
		"  -N FILE    saved-state journal (/data/milan/journal.bin), \"none\" for one in memory\n"
		"  -V VID     VLAN of the talker's streams (2)\n"
		"  -n         no SRP domain (a direct link, no MSRP bridge): a settled sink's talker counts as registered\n"
		"  -s         log to syslog\n"
		"  -v         more logging (twice: the firmware's own prints)\n"
		"SIGUSR1 logs the state; SIGTERM or SIGINT departs (ENTITY_DEPARTING) and exits.\n");
}

static int parse(int argc, char **argv)
{
	int c;
	while ((c = getopt(argc, argv, "i:e:g:d:m:N:V:nsvh")) != -1) {
		switch (c) {
		case 'i': opt.ifname = optarg; break;
		case 'e': opt.entity_path = optarg; break;
		case 'g':
			gptp_none = strcmp(optarg, "none") == 0;
			opt.gptp_shm = gptp_none ? NULL : optarg;
			break;
		case 'd': opt.dp_name = optarg; break;
		case 'm': opt.media_name = optarg; break;
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
	if (!boot()) {
		return 1;
	}
	say(LOG_INFO, "entity %016llx on %s (mac %012llx, model %016llx, %u sources, %u sinks%s): AECP open",
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
		publish_bound();
		aecp_far_poll(&far);
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
	dump();
	softfab_close(&fab);
	say(LOG_INFO, "departed");
	return 0;
}
