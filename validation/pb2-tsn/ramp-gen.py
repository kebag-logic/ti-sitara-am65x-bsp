#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic

"""Write a 32-bit per-channel counting ramp as an S32_LE WAV, to play into the
bridge with aplay and check bit for bit at the far end with ramp-check.py.

    python3 -I ramp-gen.py ramp.wav --channels 8 --rate 48000 --seconds 600
    aplay -D hw:UAC2Gadget,0 ramp.wav          # on the host, into the PB2

The --drop-frame, --repeat-frame and --flip options plant defects at known
frame indices; they exist so ramp-check.py can be tested (check V1.3).

Exit 0 = written, 2 = usage error.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import avtplib as A  # noqa: E402
from ramplib import Ramp  # noqa: E402

CHUNK = 48000


def parse_flip(s):
    try:
        f, c, b = (int(x, 0) for x in s.split(":"))
    except ValueError:
        raise argparse.ArgumentTypeError("--flip wants FRAME:CHANNEL:BIT")
    return f, c, b


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("out")
    ap.add_argument("--channels", type=int, default=8)
    ap.add_argument("--rate", type=int, default=48000)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--seconds", type=float, default=10.0)
    g.add_argument("--frames", type=int)
    ap.add_argument("--shift", type=int, default=0, help="ramp in the top 32-shift bits (8 for a 24-bit path)")
    ap.add_argument("--start", type=int, default=0, help="first frame counter")
    ap.add_argument("--lead-silence", type=int, default=0, help="zero frames before the ramp")
    ap.add_argument("--drop-frame", type=int, action="append", default=[], help="test: omit generated frame N")
    ap.add_argument("--repeat-frame", type=int, action="append", default=[], help="test: write generated frame N twice")
    ap.add_argument("--flip", type=parse_flip, action="append", default=[], help="test: FRAME:CHANNEL:BIT to invert")
    a = ap.parse_args()

    try:
        ramp = Ramp(a.channels, a.shift)
    except ValueError as e:
        A.die_usage(f"ramp-gen: {e}")
    nframes = a.frames if a.frames is not None else int(round(a.seconds * a.rate))
    if nframes <= 0:
        A.die_usage("ramp-gen: nothing to write")
    events = []
    for n in a.drop_frame:
        events.append((n, 0, "drop"))
    for n in a.repeat_frame:
        events.append((n, 1, "repeat"))
    for f, c, _b in a.flip:
        if not 0 <= c < a.channels:
            A.die_usage(f"ramp-gen: --flip channel {c} out of range")
    for n, _k, _t in events:
        if not 0 <= n < nframes:
            A.die_usage(f"ramp-gen: frame {n} is outside 0..{nframes - 1}")
    events.sort()
    flips = sorted(a.flip)

    block = a.channels * 4
    written = 0
    with open(a.out, "wb") as f:
        A.write_wav_header(f, a.rate, a.channels, 32, 0)
        if a.lead_silence:
            f.write(bytes(a.lead_silence * block))
            written += a.lead_silence
        i0 = 0
        while i0 < nframes:
            i1 = min(nframes, i0 + CHUNK)
            arr = ramp.chunk(a.start + i0, i1 - i0)
            for fr, c, b in flips:
                if i0 <= fr < i1:
                    arr[(fr - i0) * a.channels + c] ^= 1 << b
            if sys.byteorder == "big":
                arr.byteswap()
            data = memoryview(arr.tobytes())
            start = i0
            for n, _k, kind in events:
                if not i0 <= n < i1:
                    continue
                f.write(data[(start - i0) * block:(n - i0) * block])
                written += n - start
                if kind == "repeat":
                    fr = data[(n - i0) * block:(n + 1 - i0) * block]
                    f.write(fr)
                    f.write(fr)
                    written += 2
                start = n + 1
            f.write(data[(start - i0) * block:(i1 - i0) * block])
            written += i1 - start
            i0 = i1
        A.patch_wav_sizes(f, written * block)
    print(f"ramp-gen: {a.out}: {written} frames, {a.channels} ch, {a.rate} Hz, S32_LE, shift {a.shift}, "
          f"counter {a.start}..{a.start + nframes - 1}"
          + (f", planted: {len(a.drop_frame)} drop {len(a.repeat_frame)} repeat {len(a.flip)} flip" if events or flips else ""))


if __name__ == "__main__":
    main()
