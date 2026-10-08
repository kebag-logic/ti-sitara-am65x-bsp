#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic

"""Talker latency from a peer's capture (checks A3.5, A3.6, B2.4, B3.1).

Run the capture on a gPTP-synced peer whose NIC timestamps every received
frame in hardware, with the timestamps on the PHC (gPTP) time scale, e.g.

    tcpdump -i <nic> -j adapter_unsynced --time-stamp-precision=nano -w cap.pcap

Each AAF frame's AVTP timestamp is its presentation time, ingress + PTO, so

    latency = (rx_ts mod 2^32) - (avtp_timestamp - PTO)      (32-bit wrap handled)

is the time from the sample entering the talker to the frame reaching the
peer: the bridge latency plus wire and switch transit (--transit-ns removes
a separately measured transit).

    python3 -I latency-peer.py cap.pcap --pto-ns 2000000 --max-us 2000

Exit 0 = max <= --max-us and no negative latency, 1 = fail, 2 = usage error.
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import avtplib as A  # noqa: E402

RES_NS = 100  # percentile resolution


def parse_sid(s):
    s = s.lower().replace(":", "").replace("-", "")
    return int(s[2:] if s.startswith("0x") else s, 16)


def hist_percentile(hist, total, q):
    want = q / 100.0 * total
    acc = 0
    for k in sorted(hist):
        acc += hist[k]
        if acc >= want:
            return k * RES_NS
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("pcap")
    ap.add_argument("--pto-ns", type=int, default=2_000_000, help="presentation time offset of the stream")
    ap.add_argument("--transit-ns", type=int, default=0, help="measured wire/switch transit to subtract")
    ap.add_argument("--stream", action="append", default=[], help="stream ID (hex); default every AAF stream")
    ap.add_argument("--max-us", type=float, default=2000.0, help="pass if every latency is <= this")
    ap.add_argument("--min-frames", type=int, default=1)
    ap.add_argument("--allow-negative", action="store_true", help="do not fail on negative latencies")
    ap.add_argument("--bin-us", type=float, default=100.0, help="printed histogram bin width")
    ap.add_argument("--csv", help="write per-frame latencies (stream,seq,rx_ns,avtp_ts,latency_ns) here")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    if not os.path.isfile(a.pcap):
        A.die_usage(f"latency-peer: no such file {a.pcap}")
    try:
        want = {parse_sid(s) for s in a.stream}
    except ValueError as e:
        A.die_usage(f"latency-peer: {e}")

    hist = {}
    n = neg = tv0 = 0
    lo = hi = None
    per_stream = {}
    csv = open(a.csv, "w") if a.csv else None
    if csv:
        csv.write("stream_id,seq,rx_ns,avtp_ts,latency_ns\n")
    pto = a.pto_ns & 0xFFFFFFFF
    try:
        for ts, _fr, p in A.iter_avtp(a.pcap):
            if p.subtype != A.AVTP_SUBTYPE_AAF or p.avtp_ts is None:
                continue
            if want and p.stream_id not in want:
                continue
            if not p.tv:
                tv0 += 1
                continue
            lat = A.s32((ts & 0xFFFFFFFF) - ((p.avtp_ts - pto) & 0xFFFFFFFF)) - a.transit_ns
            n += 1
            per_stream[p.stream_id] = per_stream.get(p.stream_id, 0) + 1
            if lat < 0:
                neg += 1
            lo = lat if lo is None or lat < lo else lo
            hi = lat if hi is None or lat > hi else hi
            k = lat // RES_NS
            hist[k] = hist.get(k, 0) + 1
            if csv:
                csv.write(f"{p.stream_id:016x},{p.seq},{ts},{p.avtp_ts},{lat}\n")
    except A.PcapError as e:
        A.die_usage(f"latency-peer: {e}")
    finally:
        if csv:
            csv.close()

    res_ns = A.pcap_resolution_ns(a.pcap)
    pct = {q: hist_percentile(hist, n, q) for q in (50.0, 99.0, 99.9)} if n else {}
    failures = []
    if n < a.min_frames:
        failures.append(f"{n} AAF frames with tv=1 < --min-frames {a.min_frames}")
    if hi is not None and hi > a.max_us * 1000:
        failures.append(f"max latency {hi / 1000:.3f} us > {a.max_us} us")
    if neg and not a.allow_negative:
        failures.append(f"{neg} negative latencies: the peer clock is not on the talker's gPTP time "
                        "(capture with -j adapter_unsynced on a synced PHC) or the PTO is wrong")
    res = {
        "file": a.pcap, "pto_ns": a.pto_ns, "transit_ns": a.transit_ns,
        "timestamp_resolution_ns": res_ns, "frames": n, "tv0_skipped": tv0,
        "streams": {f"{k:016x}": v for k, v in per_stream.items()},
        "min_ns": lo, "p50_ns": pct.get(50.0), "p99_ns": pct.get(99.0), "p999_ns": pct.get(99.9), "max_ns": hi,
        "negative": neg, "max_us_limit": a.max_us,
        "result": "PASS" if not failures else "FAIL", "failures": failures,
    }
    if a.json:
        print(json.dumps(res, indent=2))
        sys.exit(0 if not failures else 1)

    print(f"{a.pcap}: {n} AAF frames (tv=0 skipped {tv0}), PTO {a.pto_ns} ns, transit {a.transit_ns} ns")
    if res_ns == 1000:
        print("  note: microsecond capture timestamps; latencies are good to 1 us only")
    if n:
        def us(x):
            return f"{x / 1000:.3f}" if x is not None else "-"
        print(f"  latency us: min {us(lo)} p50 {us(pct[50.0])} p99 {us(pct[99.0])} "
              f"p99.9 {us(pct[99.9])} max {us(hi)}  (percentiles to {RES_NS} ns)")
        bin_ns = max(RES_NS, int(a.bin_us * 1000))
        bins = {}
        for k, c in hist.items():
            b = (k * RES_NS) // bin_ns
            bins[b] = bins.get(b, 0) + c
        top = max(bins.values())
        for b in sorted(bins):
            bar = "#" * max(1, int(40 * bins[b] / top))
            print(f"  [{b * bin_ns / 1000:9.1f}, {(b + 1) * bin_ns / 1000:9.1f}) us {bins[b]:10d} {bar}")
    print("RESULT: " + ("PASS" if not failures else "FAIL"))
    for x in failures:
        print(f"  - {x}")
    sys.exit(0 if not failures else 1)


if __name__ == "__main__":
    main()
