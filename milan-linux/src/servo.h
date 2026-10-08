// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// servo.h - the media clock servo of the USB-to-Milan bridge (issue #10).
//
// The bridge's talker runs on the AVB media clock: 48 000 frames per gPTP
// second, one PDU of 6 frames every 125 us. The USB host delivers frames at its
// own rate. The UAC2 gadget's asynchronous feedback ("Capture Pitch 1000000")
// lets the PB2 set that rate, so the servo steers the host until the frames
// waiting in the gadget's capture buffer hold steady at a target. No sample is
// ever resampled, dropped or repeated once the servo has locked.
//
// A PI controller on the buffer level (frames). The plant is an integrator:
// level' = 48 000 * (pitch - 1 - host_offset) frames/s, with pitch in parts
// per unit, so the loop is second order. The defaults put its natural
// frequency at 0.5 rad/s with damping 0.7: about 10 s to settle from a 100 ppm
// host offset, no overshoot to speak of.

#ifndef MILAN_SERVO_H
#define MILAN_SERVO_H

#include <stdbool.h>

#define SERVO_KP_DEFAULT 14.6           // ppm of pitch per frame of level error
#define SERVO_KI_DEFAULT 5.2            // ppm per frame per second

struct servo {
	double kp, ki;
	double target;                  // frames
	double lo, hi;                  // the pitch range the gadget accepts, ppm around nominal
	double integral;                // ppm
	double out;                     // ppm: the pitch is 1e6 + out (in the gadget's 1/1e6 units)
	double lock_band;               // frames
	unsigned lock_needed;           // updates in the band before it counts as locked
	unsigned in_band;
	bool locked;
};

void servo_init(struct servo *s, double target, double lo_ppm, double hi_ppm);

// One update: `level` is the mean buffer level over the last `dt` seconds.
// Returns the pitch offset in ppm. A buffer above its target slows the host.
double servo_step(struct servo *s, double level, double dt);

#endif // MILAN_SERVO_H
