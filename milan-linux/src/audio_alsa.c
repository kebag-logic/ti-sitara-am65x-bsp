// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// audio_alsa.c - the UAC2 gadget's capture PCM (see audio_src.h).
//
// f_uac2 moves the PCM's hardware pointer at every completed USB request, so
// snd_pcm_avail() seen every 125 us shows each packet as it lands, whatever the
// ALSA period. The PCM is non-blocking and started at once: while the host
// sends nothing, it simply has nothing available.

#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <stdlib.h>

#include "audio_src.h"

#define BUFFER_FRAMES 1024u
#define PERIOD_FRAMES 48u

struct alsa_src {
	struct audio_src src;
	snd_pcm_t *pcm;
	snd_ctl_t *ctl;
	snd_ctl_elem_value_t *pitch;
};

static int restart(struct alsa_src *a)
{
	a->src.overruns++;
	int rc = snd_pcm_prepare(a->pcm);
	return rc < 0 ? rc : snd_pcm_start(a->pcm);
}

static long alsa_avail(struct audio_src *src)
{
	struct alsa_src *a = (struct alsa_src *)src;
	snd_pcm_sframes_t n = snd_pcm_avail(a->pcm);
	if (n == -EPIPE || n == -ESTRPIPE) {
		restart(a);
		return -1;
	}
	return n < 0 ? 0 : (long)n;
}

static long alsa_read(struct audio_src *src, int32_t *buf, unsigned frames)
{
	struct alsa_src *a = (struct alsa_src *)src;
	snd_pcm_sframes_t n = snd_pcm_readi(a->pcm, buf, frames);
	if (n == -EPIPE || n == -ESTRPIPE) {
		restart(a);
	}
	return (long)n;
}

static int alsa_set_pitch(struct audio_src *src, long pitch)
{
	struct alsa_src *a = (struct alsa_src *)src;
	if (a->ctl == NULL) {
		return -ENODEV;
	}
	snd_ctl_elem_value_set_integer(a->pitch, 0, pitch);
	return snd_ctl_elem_write(a->ctl, a->pitch);
}

static void alsa_close(struct audio_src *src)
{
	struct alsa_src *a = (struct alsa_src *)src;
	if (a->pcm != NULL) {
		snd_pcm_close(a->pcm);
	}
	if (a->ctl != NULL) {
		snd_ctl_close(a->ctl);
	}
	if (a->pitch != NULL) {
		snd_ctl_elem_value_free(a->pitch);
	}
	free(a);
}

// "Capture Pitch 1000000": its range, and a value to write it with.
static int open_pitch(struct alsa_src *a, const char *ctl)
{
	int rc = snd_ctl_open(&a->ctl, ctl, 0);
	if (rc < 0) {
		return rc;
	}
	snd_ctl_elem_id_t *id;
	snd_ctl_elem_info_t *info;
	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_PCM);
	snd_ctl_elem_id_set_name(id, "Capture Pitch 1000000");
	snd_ctl_elem_info_set_id(info, id);
	rc = snd_ctl_elem_info(a->ctl, info);
	if (rc < 0) {
		return rc;
	}
	a->src.pitch_min = snd_ctl_elem_info_get_min(info);
	a->src.pitch_max = snd_ctl_elem_info_get_max(info);
	rc = snd_ctl_elem_value_malloc(&a->pitch);
	if (rc < 0) {
		return rc;
	}
	snd_ctl_elem_value_set_id(a->pitch, id);
	return 0;
}

struct audio_src *audio_alsa_open(const char *pcm, const char *ctl, unsigned channels, unsigned rate)
{
	struct alsa_src *a = calloc(1, sizeof *a);
	if (a == NULL) {
		return NULL;
	}
	a->src = (struct audio_src){
		.avail = alsa_avail,
		.read = alsa_read,
		.set_pitch = alsa_set_pitch,
		.close = alsa_close,
		.channels = channels,
		.pitch_min = 1000000,
		.pitch_max = 1000000,
	};
	const char *what = "open";
	int rc = snd_pcm_open(&a->pcm, pcm, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
	if (rc >= 0) {
		what = "hw params";
		snd_pcm_hw_params_t *hw;
		snd_pcm_hw_params_alloca(&hw);
		snd_pcm_uframes_t buffer = BUFFER_FRAMES, period = PERIOD_FRAMES;
		if ((rc = snd_pcm_hw_params_any(a->pcm, hw)) >= 0 &&
		    (rc = snd_pcm_hw_params_set_access(a->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) >= 0 &&
		    (rc = snd_pcm_hw_params_set_format(a->pcm, hw, SND_PCM_FORMAT_S32_LE)) >= 0 &&
		    (rc = snd_pcm_hw_params_set_channels(a->pcm, hw, channels)) >= 0 &&
		    (rc = snd_pcm_hw_params_set_rate(a->pcm, hw, rate, 0)) >= 0 &&
		    (rc = snd_pcm_hw_params_set_period_size_near(a->pcm, hw, &period, NULL)) >= 0 &&
		    (rc = snd_pcm_hw_params_set_buffer_size_near(a->pcm, hw, &buffer)) >= 0) {
			rc = snd_pcm_hw_params(a->pcm, hw);
		}
	}
	if (rc >= 0) {
		what = "start";
		rc = snd_pcm_start(a->pcm);
	}
	if (rc >= 0 && ctl != NULL) {
		what = "Capture Pitch 1000000";
		rc = open_pitch(a, ctl);
	}
	if (rc < 0) {
		fprintf(stderr, "audio: %s: %s: %s\n", pcm, what, snd_strerror(rc));
		alsa_close(&a->src);
		return NULL;
	}
	return &a->src;
}
