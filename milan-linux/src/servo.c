// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// servo.c - see servo.h.

#include "servo.h"

void servo_init(struct servo *s, double target, double lo_ppm, double hi_ppm)
{
	*s = (struct servo){
		.kp = SERVO_KP_DEFAULT,
		.ki = SERVO_KI_DEFAULT,
		.target = target,
		.lo = lo_ppm,
		.hi = hi_ppm,
		// one USB packet: a real host delivers in microframes, with bursts
		// after a gap (measured on the PB2: +-1 frame, +5 at worst)
		.lock_band = 6.0,
		.lock_needed = 40,
	};
}

double servo_step(struct servo *s, double level, double dt)
{
	double err = level - s->target;
	double integral = s->integral - s->ki * err * dt;
	double out = integral - s->kp * err;
	// anti-windup: hold the integral while the output is pinned
	if (out > s->hi) {
		out = s->hi;
	} else if (out < s->lo) {
		out = s->lo;
	} else {
		s->integral = integral;
	}
	s->out = out;
	if (err < s->lock_band && err > -s->lock_band) {
		if (s->in_band < s->lock_needed) {
			s->in_band++;
		}
	} else {
		s->in_band = 0;
	}
	s->locked = s->in_band >= s->lock_needed;
	return out;
}
