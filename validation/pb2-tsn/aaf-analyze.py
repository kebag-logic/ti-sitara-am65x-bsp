#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic

"""Analyse the AAF streams in a capture (checks A3.1, A3.2, F3.4, F4.3).

Per stream ID: frame count and rate, sequence gaps, the tv flag, the AVTP
timestamp step and its jitter, the arrival interval jitter, and the AAF
format fields (IEEE 1722-2016 clause 7, Milan v1.2 AAF). --wav extracts the
payload of one stream as a little-endian PCM WAV for ramp-check.py.

    python3 -I aaf-analyze.py cap.pcap --expect-rate 8000 --expect-step-ns 125000 \
        --expect-format INT_32BIT --expect-channels 8 --expect-nsr 48000 --expect-spf 6

Exit 0 = every requested check passed, 1 = a check failed, 2 = usage error.
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import avtplib as A  # noqa: E402

DEV_BIN_NS = 100          # arrival deviation histogram resolution
DEV_BINS = 20000          # +/- 2 ms of deviation, then the overflow bins


class StreamState:
    def __init__(self, sid):
        self.sid = sid
        self.frames = 0
        self.first_ts = self.last_ts = None
        self.dst = self.vid = self.pcp = None
        self.prev_seq = None
        self.gaps = self.missing = self.dups = 0
        self.tv0 = 0
        self.prev_avtp = None
        self.step = A.Stats()
        self.step_bad = 0
        self.span_ns = 0
        self.span_frames = 0
        self.prev_arrival = None
        self.arrival = A.Stats()
        self.nominal = None
        self.warm = []
        self.dev_hist = [0] * (2 * DEV_BINS + 1)
        self.dev_over = 0
        self.formats = {}
        self.vlans = {}
        self.wav_bytes = 0

    def deviation_percentile(self, q):
        total = sum(self.dev_hist) + self.dev_over
        if total == 0:
            return None
        want = q / 100.0 * total
        # histogram is indexed by signed deviation; fold to |deviation|
        folded = [0] * (DEV_BINS + 1)
        for i, c in enumerate(self.dev_hist):
            folded[abs(i - DEV_BINS)] += c
        acc = 0
        for i, c in enumerate(folded):
            acc += c
            if acc >= want:
                return i * DEV_BIN_NS
        return None  # in the overflow: beyond the histogram


def parse_sid(s):
    s = s.lower().replace(":", "").replace("-", "")
    if s.startswith("0x"):
        s = s[2:]
    return int(s, 16)


def fmt_value(name):
    if name is None:
        return None
    for k, v in A.AAF_FORMAT_NAME.items():
        if v == name.upper():
            return k
    return int(name, 0)


def be_to_le(data, width):
    out = bytearray(len(data))
    for i in range(width):
        out[i::width] = data[width - 1 - i::width]
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("pcap")
    ap.add_argument("--stream", action="append", default=[], help="stream ID (hex); repeatable; default every AAF stream")
    ap.add_argument("--wav", help="extract the payload of the (single) selected stream to this WAV file")
    ap.add_argument("--json", action="store_true", help="print the summary as JSON")
    ap.add_argument("--min-frames", type=int, default=1)
    ap.add_argument("--expect-rate", type=float, help="frames/s on the capture clock")
    ap.add_argument("--rate-tol-ppm", type=float, default=100.0)
    ap.add_argument("--expect-frames", type=int, help="exact frame count, e.g. 28800000 for 1 h")
    ap.add_argument("--frames-tol", type=int, default=1)
    ap.add_argument("--expect-step-ns", type=int, help="AVTP timestamp step between consecutive frames")
    ap.add_argument("--step-tol-ns", type=int, default=1)
    ap.add_argument("--max-arrival-dev-us", type=float, help="p99.9 |arrival interval - nominal| limit")
    ap.add_argument("--expect-format", help="INT_32BIT, INT_24BIT, INT_16BIT, FLOAT_32BIT, AES3_32BIT or a number")
    ap.add_argument("--expect-channels", type=int)
    ap.add_argument("--expect-nsr", type=int, help="sample rate in Hz")
    ap.add_argument("--expect-spf", type=int, help="samples (per channel) per frame")
    ap.add_argument("--expect-bit-depth", type=int)
    ap.add_argument("--expect-vid", type=int)
    ap.add_argument("--expect-pcp", type=int)
    ap.add_argument("--max-seq-gaps", type=int, default=0)
    ap.add_argument("--allow-tv0", action="store_true", help="do not require tv=1 on every frame")
    a = ap.parse_args()

    try:
        want = {parse_sid(s) for s in a.stream}
        exp_fmt = fmt_value(a.expect_format)
    except ValueError as e:
        A.die_usage(f"aaf-analyze: {e}")
    if a.wav and len(want) != 1:
        A.die_usage("aaf-analyze: --wav needs exactly one --stream")
    if not os.path.isfile(a.pcap):
        A.die_usage(f"aaf-analyze: no such file {a.pcap}")

    streams = {}
    other_subtypes = {}
    wav_f = None
    wav_fmt = None
    nominal = a.expect_step_ns

    try:
        for ts, fr, p in A.iter_avtp(a.pcap):
            if p.subtype != A.AVTP_SUBTYPE_AAF:
                other_subtypes[p.subtype] = other_subtypes.get(p.subtype, 0) + 1
                continue
            if want and p.stream_id not in want:
                continue
            st = streams.get(p.stream_id)
            if st is None:
                st = streams[p.stream_id] = StreamState(p.stream_id)
                st.dst = A.mac_str(fr.dst)
                st.nominal = nominal
            st.frames += 1
            if st.first_ts is None:
                st.first_ts = ts
            st.last_ts = ts
            st.vlans[(fr.vid, fr.pcp)] = st.vlans.get((fr.vid, fr.pcp), 0) + 1
            key = (p.format, p.nsr, p.channels, p.bit_depth, p.sdl)
            st.formats[key] = st.formats.get(key, 0) + 1
            if not p.tv:
                st.tv0 += 1

            delta = 1
            if st.prev_seq is not None:
                delta = (p.seq - st.prev_seq) & 0xFF
                if delta == 0:
                    st.dups += 1
                elif delta != 1:
                    st.gaps += 1
                    st.missing += delta - 1
            st.prev_seq = p.seq

            if p.tv and st.prev_avtp is not None and delta > 0:
                d = (p.avtp_ts - st.prev_avtp) & 0xFFFFFFFF
                st.span_ns += d
                st.span_frames += delta
                if delta == 1:
                    st.step.add(d)
                    if a.expect_step_ns is not None and abs(d - a.expect_step_ns) > a.step_tol_ns:
                        st.step_bad += 1
            st.prev_avtp = p.avtp_ts if p.tv else None

            if st.prev_arrival is not None and delta == 1:
                iv = ts - st.prev_arrival
                st.arrival.add(iv)
                if st.nominal is None:
                    st.warm.append(iv)
                    if len(st.warm) >= 1000:
                        st.warm.sort()
                        st.nominal = st.warm[len(st.warm) // 2]
                        for w in st.warm:
                            _dev_add(st, w)
                        st.warm = []
                else:
                    _dev_add(st, iv)
            st.prev_arrival = ts

            if a.wav:
                if wav_f is None:
                    width = A.AAF_FORMAT_BYTES.get(p.format)
                    rate = A.AAF_NSR_HZ.get(p.nsr, 0)
                    if not width or not rate or not p.channels:
                        A.die_usage("aaf-analyze: stream format cannot be written as PCM WAV")
                    wav_fmt = key
                    wav_f = open(a.wav, "wb")
                    A.write_wav_header(wav_f, rate, p.channels, width * 8, 0)
                    st.wav_width = width
                elif key != wav_fmt:
                    wav_f.close()
                    A.die_usage("aaf-analyze: stream format changed mid-capture; cannot write one WAV")
                block = p.channels * st.wav_width
                usable = len(p.data) - len(p.data) % block
                wav_f.write(be_to_le(p.data[:usable], st.wav_width))
                st.wav_bytes += usable
    except A.PcapError as e:
        A.die_usage(f"aaf-analyze: {e}")

    for st in streams.values():
        if st.nominal is None and st.warm:
            st.warm.sort()
            st.nominal = st.warm[len(st.warm) // 2]
            for w in st.warm:
                _dev_add(st, w)
            st.warm = []

    if wav_f is not None:
        A.patch_wav_sizes(wav_f, next(iter(streams.values())).wav_bytes)
        wav_f.close()

    failures = []
    report = []
    if not streams:
        failures.append("no AAF stream in the capture" + (" matching --stream" if want else ""))
    for sid, st in sorted(streams.items()):
        r = summarize(st, a, failures)
        report.append(r)
    if other_subtypes:
        report.append({"other_avtp_subtypes": {f"0x{k:02x}": v for k, v in sorted(other_subtypes.items())}})

    if a.json:
        print(json.dumps({"streams": report, "failures": failures,
                          "result": "PASS" if not failures else "FAIL"}, indent=2))
    else:
        for r in report:
            print_report(r)
        if failures:
            print("RESULT: FAIL")
            for f in failures:
                print(f"  - {f}")
        else:
            print("RESULT: PASS")
    sys.exit(1 if failures else 0)


def _dev_add(st, iv):
    b = int(round((iv - st.nominal) / DEV_BIN_NS)) + DEV_BINS
    if 0 <= b < len(st.dev_hist):
        st.dev_hist[b] += 1
    else:
        st.dev_over += 1


def summarize(st, a, failures):
    sid = f"{st.sid:016x}"
    dur_ns = (st.last_ts - st.first_ts) if st.frames > 1 else 0
    rate = (st.frames - 1) / (dur_ns / 1e9) if dur_ns > 0 else None
    rate_gptp = st.span_frames / (st.span_ns / 1e9) if st.span_ns > 0 else None
    r = {
        "stream_id": sid, "dst": st.dst,
        "vlan": [{"vid": v, "pcp": p, "frames": n} for (v, p), n in st.vlans.items()],
        "frames": st.frames, "duration_s": dur_ns / 1e9,
        "rate_capture_fps": rate, "rate_avtp_fps": rate_gptp,
        "seq_gaps": st.gaps, "seq_missing": st.missing, "seq_duplicates": st.dups,
        "tv0_frames": st.tv0,
        "step_ns": {"n": st.step.n, "min": st.step.min, "max": st.step.max,
                    "mean": st.step.mean if st.step.n else None, "stdev": st.step.stdev,
                    "out_of_tol": st.step_bad},
        "arrival_ns": {"n": st.arrival.n, "min": st.arrival.min, "max": st.arrival.max,
                       "mean": st.arrival.mean if st.arrival.n else None, "nominal": st.nominal,
                       "dev_p99_ns": st.deviation_percentile(99.0),
                       "dev_p999_ns": st.deviation_percentile(99.9),
                       "dev_overflow": st.dev_over},
        "formats": [],
    }
    for (fmt, nsr, ch, bd, sdl), n in st.formats.items():
        width = A.AAF_FORMAT_BYTES.get(fmt)
        spf = sdl // (ch * width) if width and ch else None
        r["formats"].append({"format": A.AAF_FORMAT_NAME.get(fmt, f"0x{fmt:02x}"), "format_code": fmt,
                             "nsr_hz": A.AAF_NSR_HZ.get(nsr), "channels": ch, "bit_depth": bd,
                             "stream_data_length": sdl, "samples_per_frame": spf, "frames": n})

    def fail(msg):
        failures.append(f"stream {sid}: {msg}")

    if st.frames < a.min_frames:
        fail(f"{st.frames} frames < --min-frames {a.min_frames}")
    if st.gaps > a.max_seq_gaps:
        fail(f"{st.gaps} sequence gaps ({st.missing} frames missing) > {a.max_seq_gaps}")
    if st.dups:
        fail(f"{st.dups} duplicated sequence numbers")
    if st.tv0 and not a.allow_tv0:
        fail(f"{st.tv0} frames with tv=0")
    if a.expect_rate is not None:
        if rate is None:
            fail("rate needs at least two frames")
        elif abs(rate - a.expect_rate) / a.expect_rate * 1e6 > a.rate_tol_ppm:
            fail(f"rate {rate:.3f} fps differs from {a.expect_rate} by more than {a.rate_tol_ppm} ppm")
    if a.expect_frames is not None and abs(st.frames - a.expect_frames) > a.frames_tol:
        fail(f"{st.frames} frames, expected {a.expect_frames} +/-{a.frames_tol}")
    if a.expect_step_ns is not None:
        if st.step.n == 0:
            fail("no consecutive tv=1 frame pair to measure the AVTP timestamp step")
        elif st.step_bad:
            fail(f"{st.step_bad} AVTP timestamp steps outside {a.expect_step_ns} +/-{a.step_tol_ns} ns "
                 f"(min {st.step.min} max {st.step.max})")
    if a.max_arrival_dev_us is not None:
        p = r["arrival_ns"]["dev_p999_ns"]
        if p is None or p > a.max_arrival_dev_us * 1000:
            fail(f"arrival deviation p99.9 {p} ns > {a.max_arrival_dev_us} us")
    if len(st.formats) > 1:
        fail(f"format fields change within the stream ({len(st.formats)} variants)")
    for f in r["formats"]:
        if a.expect_format is not None and f["format_code"] != fmt_value(a.expect_format):
            fail(f"format {f['format']} != {a.expect_format}")
        if a.expect_channels is not None and f["channels"] != a.expect_channels:
            fail(f"channels_per_frame {f['channels']} != {a.expect_channels}")
        if a.expect_nsr is not None and f["nsr_hz"] != a.expect_nsr:
            fail(f"nsr {f['nsr_hz']} Hz != {a.expect_nsr}")
        if a.expect_spf is not None and f["samples_per_frame"] != a.expect_spf:
            fail(f"{f['samples_per_frame']} samples per frame != {a.expect_spf} "
                 f"(stream_data_length {f['stream_data_length']})")
        if a.expect_bit_depth is not None and f["bit_depth"] != a.expect_bit_depth:
            fail(f"bit_depth {f['bit_depth']} != {a.expect_bit_depth}")
        width = A.AAF_FORMAT_BYTES.get(f["format_code"])
        if width and f["channels"] and f["stream_data_length"] % (width * f["channels"]):
            fail(f"stream_data_length {f['stream_data_length']} is not a whole number of sample frames")
    for v in r["vlan"]:
        if a.expect_vid is not None and v["vid"] != a.expect_vid:
            fail(f"{v['frames']} frames on VID {v['vid']}, expected {a.expect_vid}")
        if a.expect_pcp is not None and v["pcp"] != a.expect_pcp:
            fail(f"{v['frames']} frames with PCP {v['pcp']}, expected {a.expect_pcp}")
    return r


def print_report(r):
    if "other_avtp_subtypes" in r:
        print("other AVTP stream subtypes: " +
              ", ".join(f"{k}: {v}" for k, v in r["other_avtp_subtypes"].items()))
        return
    vl = ", ".join(f"vid {v['vid']} pcp {v['pcp']} ({v['frames']})" for v in r["vlan"])
    print(f"stream {r['stream_id']}  dst {r['dst']}  {vl}")
    rc = r["rate_capture_fps"]
    ra = r["rate_avtp_fps"]
    print(f"  frames {r['frames']}  duration {r['duration_s']:.6f} s  rate "
          f"{rc:.3f} fps (capture clock)" if rc else f"  frames {r['frames']}", end="")
    print(f", {ra:.3f} fps (AVTP time)" if ra else "")
    print(f"  sequence: gaps {r['seq_gaps']} (missing {r['seq_missing']}), duplicates {r['seq_duplicates']}"
          f"  tv=0 frames {r['tv0_frames']}")
    s = r["step_ns"]
    if s["n"]:
        print(f"  avtp step ns: min {s['min']} max {s['max']} mean {s['mean']:.3f} stdev {s['stdev']:.3f}"
              f"  out-of-tol {s['out_of_tol']}")
    v = r["arrival_ns"]
    if v["n"]:
        print(f"  arrival ns: min {v['min']} max {v['max']} mean {v['mean']:.1f} nominal {v['nominal']}"
              f"  |dev| p99 {v['dev_p99_ns']} p99.9 {v['dev_p999_ns']} overflow {v['dev_overflow']}")
    for f in r["formats"]:
        print(f"  format {f['format']} nsr {f['nsr_hz']} Hz channels {f['channels']} bit_depth {f['bit_depth']}"
              f" sdl {f['stream_data_length']} -> {f['samples_per_frame']} samples/frame ({f['frames']} frames)")


if __name__ == "__main__":
    main()
