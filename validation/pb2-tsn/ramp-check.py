#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic

"""Check a recorded counting ramp bit for bit (checks F5.2, A3.4, A4.2, A5.2).

Input is a WAV written by aaf-analyze.py --wav (payload pulled off the wire)
or by arecord. Leading silence or garbage is skipped until the ramp is found;
from there every frame must carry the next counter on every channel.

Reported per file: dropped frames, repeated frames, bit errors (per channel),
silent frames inside the ramp, and the position of the first discontinuity.

    python3 -I ramp-check.py extracted.wav --min-seconds 600

Exit 0 = bit-exact, 1 = a defect (or too little ramp), 2 = usage error.
"""

import argparse
import json
import os
import sys
from array import array

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import avtplib as A  # noqa: E402
from ramplib import Ramp  # noqa: E402

CHUNK_FRAMES = 65536


def to_u32_le(data, width):
    """Widen 16/24-bit little-endian samples to 32-bit, left-justified."""
    if width == 4:
        return data
    out = bytearray(len(data) // width * 4)
    for i in range(width):
        out[4 - width + i::4] = data[i::width]
    return bytes(out)


class Checker:
    def __init__(self, ramp, width, max_events):
        self.r = ramp
        self.C = ramp.C
        self.width = width
        self.block = ramp.C * 4
        self.in_block = ramp.C * width
        self.max_events = max_events
        self.synced = False
        self.e = 0
        self.good = 0
        self.leading = 0
        self.dropped = 0
        self.repeated = 0
        self.silence = 0
        self.silence_run = 0
        self.invalid = 0
        self.bit_errors = [0] * self.C
        self.sample_errors = [0] * self.C
        self.events = []
        self.n_events = 0
        self.first_counter = None

    def event(self, pos, kind, **kw):
        self.n_events += 1
        if len(self.events) < self.max_events:
            d = {"frame": pos, "kind": kind}
            d.update(kw)
            self.events.append(d)

    def frame_values(self, buf, off):
        a = array("I")
        a.frombytes(buf[off:off + self.block])
        if sys.byteorder == "big":
            a.byteswap()
        return a

    def consensus(self, vals):
        votes = {}
        for c, v in enumerate(vals):
            d = self.r.decode(v)
            if d is not None and d[1] == c:
                votes[d[0]] = votes.get(d[0], 0) + 1
        if not votes:
            return None
        best = max(votes.values())
        cands = [n for n, k in votes.items() if k == best]
        if len(cands) > 1:
            d0 = self.r.decode(vals[0])
            if d0 is not None and d0[0] in cands:
                return d0[0]
        return cands[0]

    def count_bits(self, vals, n):
        for c, v in enumerate(vals):
            x = self.r.value(n, c)
            if v != x:
                self.sample_errors[c] += 1
                self.bit_errors[c] += bin(v ^ x).count("1")

    def slow(self, pos, vals, nxt):
        """Classify one frame that is not the expected one. nxt = next frame or None."""
        N = self.r.N
        if not any(vals):
            if self.silence_run == 0:
                self.event(pos, "silence-start", expected_counter=self.e)
            self.silence += 1
            self.silence_run += 1
            return
        self.silence_run = 0
        n = self.consensus(vals)
        nn = self.consensus(nxt) if nxt is not None else None
        if n == self.e:
            self.count_bits(vals, n)
            bad = [c for c, v in enumerate(vals) if v != self.r.value(n, c)]
            self.event(pos, "bit-error", counter=n, channels=bad,
                       bits=[(c, (vals[c] ^ self.r.value(n, c)).bit_length() - 1) for c in bad])
            self.e = (self.e + 1) % N
            self.good += 1
            return
        if nn is not None and nn == (self.e + 1) % N:
            self.count_bits(vals, self.e)
            bad = [c for c, v in enumerate(vals) if v != self.r.value(self.e, c)]
            self.event(pos, "bit-error", counter=self.e, channels=bad,
                       bits=[(c, (vals[c] ^ self.r.value(self.e, c)).bit_length() - 1) for c in bad])
            self.e = (self.e + 1) % N
            self.good += 1
            return
        if n is None:
            self.invalid += 1
            self.event(pos, "invalid", expected_counter=self.e)
            return
        if n == (self.e - 1) % N:
            self.repeated += 1
            self.event(pos, "repeat", counter=n)
            return
        d = (n - self.e) % N
        if d < N // 2:
            self.dropped += d
            self.event(pos, "drop", frames=d, first_missing_counter=self.e, resumed_at_counter=n)
        else:
            back = (self.e - n) % N
            self.repeated += back
            self.event(pos, "jump-back", frames=back, counter=n, expected_counter=self.e)
        self.count_bits(vals, n)
        self.e = (n + 1) % N
        self.good += 1

    def try_sync(self, vals, nxt):
        if not any(vals):
            return False
        n = self.consensus(vals)
        if n is None or any(v != self.r.value(n, c) for c, v in enumerate(vals)):
            return False
        if nxt is not None and self.consensus(nxt) != (n + 1) % self.r.N:
            return False
        self.synced = True
        self.first_counter = n
        self.e = (n + 1) % self.r.N
        self.good += 1
        return True

    def run(self, f, data_len):
        block = self.block
        buf = b""
        off = 0
        left = data_len
        pos = 0  # file frame index of buf[off]
        eof = False
        while True:
            if not eof and len(buf) - off < 2 * block:
                take = min(left, CHUNK_FRAMES * self.in_block)
                chunk = f.read(take) if take else b""
                left -= len(chunk)
                if not chunk:
                    eof = True
                buf = buf[off:] + self.widen(chunk)
                off = 0
            avail = (len(buf) - off) // block
            if avail == 0:
                break
            if not self.synced:
                vals = self.frame_values(buf, off)
                nxt = self.frame_values(buf, off + block) if avail > 1 else None
                if not self.try_sync(vals, nxt):
                    self.leading += 1
                off += block
                pos += 1
                continue
            # fast path: compare whole runs against the expected ramp, keep one frame for lookahead
            run = avail - 1 if avail > 1 else (1 if eof else 0)
            if run == 0:
                continue
            exp = self.r.chunk_le_bytes(self.e, run)
            if buf[off:off + run * block] == exp:
                self.e = (self.e + run) % self.r.N
                self.good += run
                self.silence_run = 0
                off += run * block
                pos += run
                continue
            lo, hi = 0, run  # first mismatching frame in [lo, hi)
            while hi - lo > 1:
                mid = (lo + hi) // 2
                if buf[off:off + mid * block] == exp[:mid * block]:
                    lo = mid
                else:
                    hi = mid
            j = lo
            if j:
                self.e = (self.e + j) % self.r.N
                self.good += j
                self.silence_run = 0
                off += j * block
                pos += j
            vals = self.frame_values(buf, off)
            nxt = self.frame_values(buf, off + block) if (len(buf) - off) // block > 1 else None
            self.slow(pos, vals, nxt)
            off += block
            pos += 1
        # silence that runs to the end is the player stopping, not a dropout
        trailing = self.silence_run
        self.silence -= trailing
        if trailing and self.events and self.events[-1]["kind"] == "silence-start":
            self.events.pop()
            self.n_events -= 1
        return pos, trailing

    def widen(self, chunk):
        return to_u32_le(chunk, self.width) if self.width != 4 else chunk


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("wav")
    ap.add_argument("--shift", type=int, default=0, help="as given to ramp-gen.py")
    ap.add_argument("--min-seconds", type=float, default=0.0, help="fail if less ramp than this was checked")
    ap.add_argument("--allow-silence", action="store_true", help="silent frames inside the ramp do not fail")
    ap.add_argument("--max-report", type=int, default=20, help="events listed in the report")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    try:
        info = A.read_wav_info(a.wav)
    except (OSError, ValueError) as e:
        A.die_usage(f"ramp-check: {a.wav}: {e}")
    if info.bits not in (16, 24, 32):
        A.die_usage(f"ramp-check: {info.bits}-bit samples are not supported")
    try:
        ramp = Ramp(info.channels, a.shift)
    except ValueError as e:
        A.die_usage(f"ramp-check: {e}")
    ck = Checker(ramp, info.bits // 8, a.max_report)
    with open(a.wav, "rb") as f:
        f.seek(info.data_offset)
        total, trailing = ck.run(f, info.data_len - info.data_len % ck.in_block)

    checked_s = ck.good / info.rate if info.rate else 0.0
    failures = []
    if not ck.synced:
        failures.append("no ramp found")
    if ck.dropped:
        failures.append(f"{ck.dropped} dropped frames")
    if ck.repeated:
        failures.append(f"{ck.repeated} repeated frames")
    if sum(ck.bit_errors):
        failures.append(f"{sum(ck.bit_errors)} bit errors in {sum(ck.sample_errors)} samples")
    if ck.invalid:
        failures.append(f"{ck.invalid} frames that are not ramp values")
    if ck.silence and not a.allow_silence:
        failures.append(f"{ck.silence} silent frames inside the ramp")
    if checked_s < a.min_seconds:
        failures.append(f"{checked_s:.3f} s of ramp checked < --min-seconds {a.min_seconds}")

    res = {
        "file": a.wav, "rate": info.rate, "channels": info.channels, "bits": info.bits, "shift": a.shift,
        "frames_in_file": total, "leading_skipped": ck.leading, "trailing_silence": trailing,
        "first_counter": ck.first_counter, "ramp_frames_checked": ck.good, "ramp_seconds_checked": checked_s,
        "dropped_frames": ck.dropped, "repeated_frames": ck.repeated, "silent_frames_inside": ck.silence,
        "invalid_frames": ck.invalid,
        "bit_errors_per_channel": ck.bit_errors, "sample_errors_per_channel": ck.sample_errors,
        "events_total": ck.n_events, "events": ck.events,
        "first_discontinuity": ck.events[0] if ck.events else None,
        "result": "PASS" if not failures else "FAIL", "failures": failures,
    }
    if a.json:
        print(json.dumps(res, indent=2))
    else:
        print(f"{a.wav}: {info.channels} ch, {info.bits}-bit, {info.rate} Hz, shift {a.shift}")
        print(f"  frames {total}: skipped {ck.leading} leading, {trailing} trailing silent; "
              f"ramp from counter {ck.first_counter}, {ck.good} frames checked ({checked_s:.3f} s)")
        print(f"  dropped {ck.dropped}  repeated {ck.repeated}  silent-inside {ck.silence}  invalid {ck.invalid}"
              f"  bit errors {sum(ck.bit_errors)} (per channel {ck.bit_errors})")
        if ck.events:
            print(f"  events ({ck.n_events}, first {len(ck.events)} listed; frame = index in the file):")
            for ev in ck.events:
                rest = " ".join(f"{k}={v}" for k, v in ev.items() if k not in ("frame", "kind"))
                t = ev["frame"] / info.rate if info.rate else 0
                print(f"    frame {ev['frame']} ({t:.6f} s) {ev['kind']} {rest}")
        print("RESULT: " + ("PASS" if not failures else "FAIL"))
        for x in failures:
            print(f"  - {x}")
    sys.exit(0 if not failures else 1)


if __name__ == "__main__":
    main()
