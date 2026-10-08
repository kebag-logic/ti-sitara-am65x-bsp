// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// audio_src.h - where the talker's frames come from: the UAC2 gadget's
// capture PCM (the host's USB OUT data) on the board, or a simulated USB host
// in the host tests. Interleaved int32 frames, the gadget's S32_LE.
//
// The pitch is the gadget's asynchronous feedback, "Capture Pitch 1000000":
// the host sends 48 000 * pitch / 1e6 frames per second of USB bus time.

#ifndef MILAN_AUDIO_SRC_H
#define MILAN_AUDIO_SRC_H

#include <stdint.h>

struct audio_src {
	// Frames ready now; negative after an overrun the source recovered from
	// (the count is lost, the stream goes on).
	long (*avail)(struct audio_src *s);
	// Take `frames` frames; returns the frames taken, or a negative errno.
	long (*read)(struct audio_src *s, int32_t *buf, unsigned frames);
	// Ask the host for 48 000 * pitch / 1e6 frames/s (clamped by the source).
	int (*set_pitch)(struct audio_src *s, long pitch);
	void (*close)(struct audio_src *s);
	unsigned channels;
	long pitch_min, pitch_max;
	unsigned long overruns;
};

// The gadget's capture PCM `pcm` (e.g. hw:CARD=UAC2Gadget,DEV=0) and its
// control device `ctl` (hw:CARD=UAC2Gadget). NULL with a message on stderr.
struct audio_src *audio_alsa_open(const char *pcm, const char *ctl, unsigned channels, unsigned rate);

// A USB host whose bus clock is host_ppm off gPTP, honouring the pitch like
// the gadget's feedback, sending the validation kit's counting ramp
// (validation/pb2-tsn/ramplib.py, shift 0) in 125 us microframes.
struct audio_src *audio_sim_open(unsigned channels, unsigned rate, double host_ppm);

#endif // MILAN_AUDIO_SRC_H
