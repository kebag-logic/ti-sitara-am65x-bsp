// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// aecp_far.c - the datapath behind the face block (see aecp_far.h).

#include "aecp_far.h"

#include <string.h>

#include "mbx_contract.h"
#include "mbx_model.h"

// IEEE 1722.1-2021 Table 7-1
enum { STREAM_INPUT = 5u, STREAM_OUTPUT = 6u, AVB_INTERFACE = 9u, CLOCK_DOMAIN = 36u };

// IEEE 1722.1-2021 Table 7-145 (bits numbered from the most significant), the
// STREAM_OUTPUT flags this side reports
#define SI_STREAM_FORMAT_VALID 0x80000000u
#define SI_STREAM_ID_VALID 0x40000000u
#define SI_STREAM_DEST_MAC_VALID 0x10000000u
#define SI_STREAM_VLAN_ID_VALID 0x02000000u
// IEEE 1722.1-2021 Table 7-148, GET_AVB_INFO flags
#define AVB_AS_CAPABLE 0x01u
#define AVB_GPTP_ENABLED 0x02u

// counters_valid: bit k for quadlet k (IEEE 1722.1-2021 7.4.42.2; Milan v1.2
// Tables 5.13 to 5.15; the streams' in media.h, Tables 5.16 and 5.17)
enum { AVB_LINK_UP = 0, AVB_LINK_DOWN = 1, AVB_GM_CHANGED = 5 };
enum { CD_LOCKED = 0, CD_UNLOCKED = 1 };

#define MEDIA_POLL_MS 100u

static uint32_t field(uint32_t word, unsigned lsb, unsigned width)
{
	return (word >> lsb) & ((width >= 32u ? 0u : 1u << width) - 1u);
}

static void put(struct aecp_far_counters *c, unsigned quadlet, uint64_t value)
{
	c->valid |= 1u << quadlet;
	c->value[quadlet] = (uint32_t)value;    // a counter's 32 bits wrap
}

// A media program's Milan counters for one stream, as it counts them: valid
// only for the quadlets it marks valid, none while milan-mediad's block is
// absent. A STREAM_INPUT's belong to one binding (media.h): while the
// datapath block shows a later not-bound to bound change than the one they
// belong to, milan-mediad has not yet zeroed them for the new binding, and
// for that binding it has counted nothing, so they read zero.
static struct aecp_far_counters stream_counters(const struct aecp_far *f, const struct milan_counters *m, int sink)
{
	struct aecp_far_counters c = {0};
	if (!f->media_valid) {
		return c;
	}
	uint32_t binds = m->binds;
	struct milan_dp dp;
	if (sink >= 0 && milan_dp_snapshot(f->dp, &dp) && (unsigned)sink < dp.n_sinks) {
		binds = dp.sinks[sink].binds;
	}
	for (unsigned k = 0; k < MILAN_COUNTERS; ++k) {
		if (m->valid & (1u << k)) {
			put(&c, k, binds == m->binds ? m->value[k] : 0u);
		}
	}
	return c;
}

// Which media program counts which stream: milan-mediad's AAF listener
// STREAM_INPUT 0, its CRF receive path STREAM_INPUT 1 (crf_rx.h), its talker
// STREAM_OUTPUT 0. Any other stream of the model has none.
static void stream_poll(struct aecp_far *f, struct aecp_far_counters *in, struct aecp_far_counters *out)
{
	memset(in, 0, sizeof *in * AECP_FAR_COUNTED);
	memset(out, 0, sizeof *out * AECP_FAR_COUNTED);
	const struct milan_media *m = &f->media_copy;
	if (f->cfg.n_inputs > 0u) {
		in[0] = stream_counters(f, &m->listener.milan, 0);
	}
	if (f->cfg.n_inputs > 1u) {
		in[1] = stream_counters(f, &m->crf.milan, 1);
	}
	if (f->cfg.n_outputs > 0u) {
		out[0] = stream_counters(f, &m->talker.milan, -1);
	}
}

static bool counters(const struct aecp_far *f, uint16_t type, uint16_t index, struct aecp_far_counters *out)
{
	memset(out, 0, sizeof *out);
	if (type == AVB_INTERFACE && index == 0u) {
		put(out, AVB_LINK_UP, f->link_ups);
		put(out, AVB_LINK_DOWN, f->link_downs);
		put(out, AVB_GM_CHANGED, f->gm_changes);
	} else if (type == CLOCK_DOMAIN && index == 0u) {
		put(out, CD_LOCKED, f->locks);
		put(out, CD_UNLOCKED, f->unlocks);
	} else if (type == STREAM_INPUT && index < f->cfg.n_inputs) {
		if (index < AECP_FAR_COUNTED) {
			*out = f->input[index];
		}
	} else if (type == STREAM_OUTPUT && index < f->cfg.n_outputs) {
		if (index < AECP_FAR_COUNTED) {
			*out = f->output[index];
		}
	} else {
		return false;
	}
	return true;                    // a stream nothing counts: answered, none valid
}

// GET_STREAM_INFO's words (aecp_face.c stream): 0 flags, 2 stream_id,
// 3 msrp_accumulated_latency, 4 {dest_mac, failure_code}, 5 failure_bridge_id,
// 6 {stream_vlan_id, flags_ex}; selector 15, SET_STREAM_FORMAT's verdict.
static bool stream_word(const struct aecp_far *f, uint16_t type, uint16_t index, unsigned sel, uint64_t proposed,
			uint64_t *data)
{
	bool input = type == STREAM_INPUT;
	if ((!input && type != STREAM_OUTPUT) || index >= (input ? f->cfg.n_inputs : f->cfg.n_outputs)) {
		return false;
	}
	if (sel == 15u) {
		// bit 0 supported, bit 1 every mapped channel inside it: the one format the
		// media plane runs, whose channels are every mapped one
		*data = proposed == (input ? f->cfg.input_format[index] : f->cfg.output_format[index]) ? 3u : 0u;
		return true;
	}
	if (input) {
		*data = 0;                  // the bridge answers from ACMP's view (ctrl_app_aecp.c)
		return true;
	}
	struct milan_dp dp;
	if (!milan_dp_snapshot(f->dp, &dp) || index >= dp.n_sources || index >= MILAN_DP_MAX_SOURCES) {
		return false;
	}
	const struct milan_dp_source *s = &dp.sources[index];
	switch (sel) {
	case 0:
		*data = SI_STREAM_FORMAT_VALID | SI_STREAM_ID_VALID | SI_STREAM_VLAN_ID_VALID |
			(s->dest_mac_valid ? SI_STREAM_DEST_MAC_VALID : 0u);
		break;
	case 2: *data = s->stream_id; break;
	case 4: *data = s->dest_mac_valid ? (s->dest_mac & 0xFFFFFFFFFFFFull) << 16 : 0u; break;
	case 6: *data = (uint64_t)s->vlan_id << 48; break;
	default: *data = 0; break;      // 3 no MSRP latency, 5 no failure bridge
	}
	return true;
}

// GET_AVB_INFO's words (aecp_face.c avb): 0 gptp_grandmaster_id,
// 1 {propagation_delay, domain, flags, msrp_mappings_count}, 8 the SR class A
// mapping {priority, vlan_id}.
static bool avb_word(const struct aecp_far *f, uint16_t index, unsigned sel, uint64_t *data)
{
	const struct softfab *sf = f->fab;
	if (index != 0u) {
		return false;
	}
	if (sel == 0u) {
		*data = sf->gm_known ? sf->gm_id : 0u;
	} else if (sel == 1u) {
		int64_t delay = sf->gptp_seen ? sf->gptp_last.mean_link_delay_ns : 0;
		uint32_t pd = delay <= 0 ? 0u : delay > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)delay;
		uint32_t flags = (sf->as_capable ? AVB_AS_CAPABLE : 0u) | (sf->gptp_open ? AVB_GPTP_ENABLED : 0u);
		*data = (uint64_t)pd << 32 | (uint64_t)0u << 24 | (uint64_t)flags << 16 | 1u;
	} else if (sel == 8u) {
		*data = (uint64_t)f->cfg.class_a_priority << 16 | f->cfg.vlan;
	} else {
		*data = 0;
	}
	return true;
}

// GET_AS_PATH's words (aecp_face.c path): 0 the count, 8 the entry at the
// ordinal. flexptpd publishes no path trace, only the grandmaster, its own
// identity and the master's stepsRemoved, so the path is answered where those
// determine it (IEEE 802.1AS-2011 10.3.8.21, pathTrace: the grandmaster first,
// each time-aware system after it) and the window is silent anywhere else.
static bool path_word(const struct aecp_far *f, uint16_t index, unsigned sel, unsigned ord, uint64_t *data)
{
	const struct softfab *sf = f->fab;
	if (index != 0u || !sf->gptp_seen || !sf->gptp_last.gm_present) {
		return false;
	}
	uint64_t own = sf->gptp_last.own_identity, gm = sf->gptp_last.gm_identity;
	uint64_t path[2];
	unsigned n = 0;
	if (gm == own) {
		path[n++] = own;
	} else if (sf->gptp_last.steps_removed == 0u) {
		path[n++] = gm;
		path[n++] = own;
	} else {
		return false;
	}
	if (sel == 0u) {
		*data = n;
		return true;
	}
	if (sel == 8u && ord < n) {
		*data = path[ord];
		return true;
	}
	return false;
}

static enum mbx_model_reply answer(void *ctx, unsigned interface, unsigned window, const uint32_t *r,
				   uint64_t *data)
{
	struct aecp_far *f = ctx;
	bool ok = false;
	*data = 0;
	if (interface == 0u && window == MBX_MODEL_WIN_GSI) {
		uint16_t type = (uint16_t)field(r[0], MBX_GSI_REQ_TYPE_LSB, MBX_GSI_REQ_TYPE_WIDTH);
		uint16_t index = (uint16_t)field(r[0], MBX_GSI_REQ_INDEX_LSB, MBX_GSI_REQ_INDEX_WIDTH);
		unsigned kind = field(r[1], MBX_GSI_SEL_KIND_LSB, MBX_GSI_SEL_KIND_WIDTH);
		unsigned sel = field(r[1], MBX_GSI_SEL_SEL_LSB, MBX_GSI_SEL_SEL_WIDTH);
		unsigned ord = field(r[1], MBX_GSI_SEL_ORD_LSB, MBX_GSI_SEL_ORD_WIDTH);
		uint64_t proposed = (uint64_t)r[3] << 32 | r[2];
		ok = kind == 0u ? stream_word(f, type, index, sel, proposed, data)
		   : kind == 1u ? type == AVB_INTERFACE && avb_word(f, index, sel, data)
		   : kind == 2u ? type == AVB_INTERFACE && path_word(f, index, sel, ord, data)
		   : false;
	} else if (interface == 0u && window == MBX_MODEL_WIN_CTR) {
		uint16_t type = (uint16_t)field(r[0], MBX_CTR_REQ_TYPE_LSB, MBX_CTR_REQ_TYPE_WIDTH);
		uint16_t index = (uint16_t)field(r[0], MBX_CTR_REQ_INDEX_LSB, MBX_CTR_REQ_INDEX_WIDTH);
		unsigned word = field(r[1], MBX_CTR_WORD_WORD_LSB, MBX_CTR_WORD_WORD_WIDTH);
		struct aecp_far_counters c;
		ok = word <= 32u && counters(f, type, index, &c);
		if (ok) {
			*data = word == 32u ? c.valid : c.value[word];
		}
	} else if (interface == 0u && window == MBX_MODEL_WIN_EDIT) {
		// every beat answered, a begin refused (bit 0 clear): no map edit is
		// applied, and the core answers NOT_SUPPORTED (aecp_face.c edit)
		f->refused_edits += field(r[1], MBX_EDIT_BEAT_PHASE_LSB, MBX_EDIT_BEAT_PHASE_WIDTH) == 0u ? 1u : 0u;
		ok = true;
	}
	f->answers += ok ? 1u : 0u;
	f->silent += ok ? 0u : 1u;
	return ok ? MBX_MODEL_ANSWER : MBX_MODEL_SILENT;
}

static bool commit(void *ctx, unsigned interface, const uint32_t *block)
{
	struct aecp_far *f = ctx;
	(void)interface;
	(void)block;
	f->commits++;
	return true;
}

void aecp_far_init(struct aecp_far *f, struct softfab *fab, const struct milan_dp *dp,
		   const struct aecp_far_config *cfg)
{
	static struct mbx_model_far side;
	memset(f, 0, sizeof *f);
	f->cfg = *cfg;
	f->fab = fab;
	f->dp = dp;
	side = (struct mbx_model_far){f, answer, commit};
	mbx_model_far_bind(&fab->model, &side);
}

static void raise_counters(struct aecp_far *f, uint16_t type, uint16_t index)
{
	mbx_model_face_event(&f->fab->model, 0, MBX_MODEL_EV_COUNTERS, type, index);
}

void aecp_far_poll(struct aecp_far *f)
{
	struct softfab *sf = f->fab;
	// the AVB_INTERFACE: link transitions, grandmaster and asCapable
	if (sf->link_up != f->link_up) {
		f->link_up = sf->link_up;
		if (f->link_up) {
			f->link_ups++;
		} else {
			f->link_downs++;
		}
		raise_counters(f, AVB_INTERFACE, 0);
		mbx_model_face_event(&sf->model, 0, MBX_MODEL_EV_AVB, 0, 0);
	}
	uint64_t gm = sf->gm_known ? sf->gm_id : 0u;
	if (gm != f->gm_id || sf->as_capable != f->as_capable) {
		if (gm != f->gm_id && gm != 0u) {
			f->gm_changes++;
			raise_counters(f, AVB_INTERFACE, 0);
		}
		f->gm_id = gm;
		f->as_capable = sf->as_capable;
		mbx_model_face_event(&sf->model, 0, MBX_MODEL_EV_AVB, 0, 0);
		mbx_model_face_event(&sf->model, 0, MBX_MODEL_EV_ASP, 0, 0);
	}
	// the CLOCK_DOMAIN: the PHC's lock to the grandmaster, which the media clock follows
	bool locked = sf->gptp_seen && sf->gptp_last.locked;
	if (locked != f->gptp_locked) {
		f->gptp_locked = locked;
		if (locked) {
			f->locks++;
		} else {
			f->unlocks++;
		}
		raise_counters(f, CLOCK_DOMAIN, 0);
	}
	// the streams: milan-mediad's block, every MEDIA_POLL_MS
	uint64_t now = softfab_now_ms(sf);
	if (now < f->next_media_ms) {
		return;
	}
	f->next_media_ms = now + MEDIA_POLL_MS;
	if (f->media == NULL && f->cfg.media_name != NULL) {
		f->media = milan_media_open(f->cfg.media_name);
	}
	f->media_valid = f->media != NULL && milan_media_snapshot(f->media, &f->media_copy);
	struct aecp_far_counters in[AECP_FAR_COUNTED], out[AECP_FAR_COUNTED];
	stream_poll(f, in, out);
	for (unsigned k = 0; k < AECP_FAR_COUNTED; ++k) {
		if (memcmp(&in[k], &f->input[k], sizeof in[k]) != 0) {
			f->input[k] = in[k];
			raise_counters(f, STREAM_INPUT, (uint16_t)k);
		}
		if (memcmp(&out[k], &f->output[k], sizeof out[k]) != 0) {
			f->output[k] = out[k];
			raise_counters(f, STREAM_OUTPUT, (uint16_t)k);
		}
	}
}
