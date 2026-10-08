#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic

"""Regenerate the committed fixtures the self-tests judge.

Every fixture is synthetic and deterministic (fixed seeds): the tool outputs
(cyclictest, ptp4l, pmc, tc, ethtool) follow the formats those tools print,
and the captures are built frame by frame. Replace a text fixture with a real
capture from the bench whenever one is available.

    python3 -I make-samples.py            # rewrite every fixture next to this file
    python3 -I make-samples.py --v12 DIR  # the V1.2 known-latency captures, into DIR

Exit 0 = written, 2 = usage error.
"""

import argparse
import math
import os
import random
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.dirname(HERE)
sys.path.insert(0, KIT)
import avtplib as A  # noqa: E402
from ramplib import Ramp  # noqa: E402

DST = bytes.fromhex("91e0f000fe00")
SRC = bytes.fromhex("020000000012")
SID_8CH = 0x020000FFFE000012
SID_2CH = 0x020000FFFE000013
PTO = 2_000_000
STEP = 125_000
SPF = 6


def write(name, text):
    path = os.path.join(HERE, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


# ------------------------------------------------------------ cyclictest ----

def cyclictest(maxes, total=18_000_000, truncated=False):
    rnd = random.Random(1)
    ncpu = len(maxes)
    out = ["# /dev/cpu_dma_latency set to 0us", "# Histogram"]
    for us in range(0, 400):
        row = []
        for c in range(ncpu):
            v = int(total * math.exp(-((us - 8) ** 2) / 8.0) / 5.0) if us < 30 else 0
            if us == maxes[c]:
                v = max(v, 1)
            row.append(v)
        out.append(f"{us:06d} " + "\t".join(f"{v:06d}" for v in row))
    if truncated:
        return "\n".join(out[:120]) + "\n"
    out.append("# Total: " + " ".join(f"{total:09d}" for _ in maxes))
    out.append("# Min Latencies: " + " ".join(f"{rnd.randint(3, 5):05d}" for _ in maxes))
    out.append("# Avg Latencies: " + " ".join(f"{rnd.randint(7, 11):05d}" for _ in maxes))
    out.append("# Max Latencies: " + " ".join(f"{m:05d}" for m in maxes))
    out.append("# Histogram Overflows: " + " ".join("00000" for _ in maxes))
    out.append("# Histogram Overflow at cycle number:")
    for c in range(ncpu):
        out.append(f"# Thread {c}:")
    return "\n".join(out) + "\n"


# ----------------------------------------------------------------- ptp4l ----

def ptp4l_log(kind):
    rnd = random.Random({"pass": 2, "fail": 3, "summary": 4}[kind])
    t = 100.0
    L = []

    def line(msg, dt=0.0):
        nonlocal t
        t += dt
        L.append(f"ptp4l[{t:.3f}]: {msg}")

    line("selected /dev/ptp0 as PTP clock")
    line("port 1 (eth0): INITIALIZING to LISTENING on INIT_COMPLETE", 0.001)
    line("port 0 (/var/run/ptp4l): INITIALIZING to LISTENING on INIT_COMPLETE", 0.001)
    line("port 0 (/var/run/ptp4lro): INITIALIZING to LISTENING on INIT_COMPLETE", 0.001)
    line("port 1 (eth0): new foreign master 3cc0c6.fffe.fe0210-1", 1.5)
    line("selected best master clock 3cc0c6.fffe.fe0210", 4.0)
    line("port 1 (eth0): LISTENING to UNCALIBRATED on RS_SLAVE", 0.0)
    for off in (-41234, -9876, -1650):  # before the lock: must not be judged
        line(f"master offset {off:10d} s2 freq {-17000 + off // 100:+7d} path delay {380:9d}", 0.125)
    line("port 1 (eth0): UNCALIBRATED to SLAVE on MASTER_CLOCK_SELECTED", 0.125)
    if kind == "summary":
        for _ in range(120):
            r = rnd.randint(2, 9)
            line(f"rms {r:4d} max {r + rnd.randint(2, 20):4d} freq {-17354 + rnd.randint(-5, 5):+6d} +/- "
                 f"{rnd.randint(1, 6):3d} delay {380 + rnd.randint(-3, 3):5d} +/- {rnd.randint(0, 2):3d}", 1.0)
        return "\n".join(L) + "\n"
    for i in range(8 * 120):
        off = int(rnd.gauss(0, 15))
        off = max(-60, min(60, off))
        dl = 380 + rnd.randint(-8, 8)
        if kind == "fail" and i == 500:
            off = 1500
        line(f"master offset {off:10d} s2 freq {-17354 + rnd.randint(-6, 6):+7d} path delay {dl:9d}", 0.125)
        if kind == "fail" and i == 700:
            line("port 1 (eth0): SLAVE to UNCALIBRATED on SYNCHRONIZATION_FAULT", 0.0)
            line("port 1 (eth0): UNCALIBRATED to SLAVE on MASTER_CLOCK_SELECTED", 0.5)
    return "\n".join(L) + "\n"


def pmc_output(slave):
    me = "020000.fffe.000012-1"
    gm = "3cc0c6.fffe.fe0210"
    st = "SLAVE" if slave else "LISTENING"
    return f"""sending: GET PORT_DATA_SET
\t{me} seq 0 RESPONSE MANAGEMENT PORT_DATA_SET
\t\tportIdentity            {me}
\t\tportState               {st}
\t\tlogMinDelayReqInterval  0
\t\tpeerMeanPathDelay       {380 if slave else 0}
\t\tlogAnnounceInterval     0
\t\tannounceReceiptTimeout  3
\t\tlogSyncInterval         -3
\t\tdelayMechanism          2
\t\tlogMinPdelayReqInterval 0
\t\tversionNumber           2
sending: GET PORT_DATA_SET_NP
\t{me} seq 1 RESPONSE MANAGEMENT PORT_DATA_SET_NP
\t\tneighborPropDelayThresh 800
\t\tasCapable               {1 if slave else 0}
sending: GET TIME_STATUS_NP
\t{me} seq 2 RESPONSE MANAGEMENT TIME_STATUS_NP
\t\tmaster_offset              {-3 if slave else 0}
\t\tingress_time               {1791450000123456789 if slave else 0}
\t\tcumulativeScaledRateOffset +0.000000000
\t\tscaledLastGmPhaseChange    0
\t\tgmTimeBaseIndicator        0
\t\tlastGmPhaseChange          0x0000'0000000000000000.0000
\t\tgmPresent                  {"true" if slave else "false"}
\t\tgmIdentity                 {gm if slave else me[:-2]}
sending: GET PARENT_DATA_SET
\t{me} seq 3 RESPONSE MANAGEMENT PARENT_DATA_SET
\t\tparentPortIdentity                    {gm + "-1" if slave else me}
\t\tparentStats                           0
\t\tobservedParentOffsetScaledLogVariance 0xffff
\t\tobservedParentClockPhaseChangeRate    0x7fffffff
\t\tgrandmasterPriority1                  {246 if slave else 248}
\t\tgm.ClockClass                         {6 if slave else 248}
\t\tgm.ClockAccuracy                      0x20
\t\tgm.OffsetScaledLogVariance            0x4e5d
\t\tgrandmasterPriority2                  248
\t\tgrandmasterIdentity                   {gm if slave else me[:-2]}
"""


# ---------------------------------------------------------------- shaper ----

def qdisc(offloaded=True):
    off = " offloaded" if offloaded else ""
    return f"""qdisc mqprio 100: root refcnt 9{off} tc 3 map 0 0 1 2 0 0 0 0 0 0 0 0 0 0 0 0
             queues:(0:0) (1:1) (2:2)
             mode:channel
             shaper:bw_rlimit\tmin_rate:0bit 1Mbit 17Mbit \tmax_rate:0bit 2Mbit 20Mbit
 Sent 98765432 bytes 412345 pkt (dropped 0, overlimits 0 requeues 0)
 backlog 0b 0p requeues 0
qdisc pfifo_fast 0: parent 100:3 bands 3 priomap 1 2 2 2 1 2 0 0 1 1 1 1 1 1 1 1
 Sent 4950000 bytes 21153 pkt (dropped 0, overlimits 0 requeues 0)
 backlog 0b 0p requeues 0
qdisc pfifo_fast 0: parent 100:2 bands 3 priomap 1 2 2 2 1 2 0 0 1 1 1 1 1 1 1 1
 Sent 120000 bytes 1000 pkt (dropped 0, overlimits 0 requeues 0)
 backlog 0b 0p requeues 0
qdisc pfifo_fast 0: parent 100:1 bands 3 priomap 1 2 2 2 1 2 0 0 1 1 1 1 1 1 1 1
 Sent 93695432 bytes 390192 pkt (dropped 0, overlimits 0 requeues 0)
 backlog 0b 0p requeues 0
"""


def privflags(rrobin):
    return f"Private flags for eth0:\np0-rx-ptype-rrobin: {'on' if rrobin else 'off'}\n"


def ethtool_s(pri):
    host = ["p0_rx_good_frames", "p0_rx_multicast_frames", "p0_tx_good_frames"]
    lines = ["NIC statistics:"]
    for k in host:
        lines.append(f"     {k}: 123456")
    for p in range(8):
        lines.append(f"     p0_tx_pri{p}: {1000 * p}")
    lines.append("     rx_good_frames: 445566")
    lines.append("     tx_good_frames: 778899")
    for p in range(8):
        lines.append(f"     tx_pri{p}: {pri.get(p, 0)}")
    for p in range(8):
        lines.append(f"     tx_pri{p}_bcnt: {pri.get(p, 0) * 238}")
    for p in range(8):
        lines.append(f"     tx_pri{p}_drop: 0")
    return "\n".join(lines) + "\n"


def shaper_dir(name, offloaded=True, rrobin=False, move=(0, 1, 2)):
    before = {0: 500000, 1: 1000, 2: 80000}
    after = dict(before)
    for p in move:
        after[p] += {0: 812000, 1: 2000, 2: 80000}[p]
    write(f"shaper-{name}/qdisc.txt", qdisc(offloaded))
    write(f"shaper-{name}/privflags.txt", privflags(rrobin))
    write(f"shaper-{name}/stats-before.txt", ethtool_s(before))
    write(f"shaper-{name}/stats-after.txt", ethtool_s(after))


# -------------------------------------------------------------- captures ----

def aaf_stream(n_frames, channels, sid, rnd, latency_ns, t0_ns, gap_at=None, tv0_at=(), bad_step_at=None):
    """Frames of one AAF stream carrying a counting ramp, with their peer RX times."""
    ramp = Ramp(channels)
    recs = []
    for i in range(n_frames):
        ingress = t0_ns + i * STEP
        avtp = ingress + PTO
        if bad_step_at is not None and i == bad_step_at:
            avtp += 50
        if gap_at is not None and i == gap_at:
            continue
        vals = ramp.chunk(i * SPF, SPF)
        payload = b"".join(struct.pack("!I", v) for v in vals)
        fr = A.build_aaf_frame(DST, SRC, sid, i, avtp, payload, channels,
                               tv=0 if i in tv0_at else 1)
        lat = latency_ns(i) if callable(latency_ns) else latency_ns
        rx = ingress + lat + rnd.randint(-2000, 2000)
        recs.append((rx, fr))
    return recs


def other_frames(t0):
    gptp = bytes.fromhex("0180c200000e") + SRC + struct.pack("!H", 0x88F7) + bytes(44)
    adp = bytes.fromhex("91e0f0010000") + SRC + struct.pack("!H", A.ETH_P_AVTP) + bytes([0xFA, 0x01]) + bytes(66)
    crf = DST + SRC + struct.pack("!HH", A.ETH_P_8021Q, (3 << 13) | 2) + struct.pack("!H", A.ETH_P_AVTP) \
        + bytes([A.AVTP_SUBTYPE_CRF, 0x81]) + bytes(18) + bytes(48)
    return [(t0 + 10_000, gptp), (t0 + 20_000, adp), (t0 + 30_000, crf)]


def captures():
    rnd = random.Random(5)
    t0 = 1_791_450_000_000_000_000
    recs = aaf_stream(400, 8, SID_8CH, rnd, 400_000, t0) + other_frames(t0)
    recs.sort(key=lambda r: r[0])
    A.write_pcap(os.path.join(HERE, "aaf-pass.pcap"), recs)

    rnd = random.Random(6)
    recs = aaf_stream(400, 2, SID_2CH, rnd, 400_000, t0, gap_at=200, tv0_at=(300, 301), bad_step_at=350)
    A.write_pcap(os.path.join(HERE, "aaf-fail.pcap"), recs)

    rnd = random.Random(7)
    recs = aaf_stream(400, 2, SID_2CH, rnd, lambda i: 300_000 + (i * 7919) % 600_000, t0)
    A.write_pcap(os.path.join(HERE, "latency-pass.pcap"), recs)
    rnd = random.Random(8)
    recs = aaf_stream(400, 2, SID_2CH, rnd, lambda i: 2_500_000 if i == 123 else 300_000 + (i * 7919) % 600_000, t0)
    A.write_pcap(os.path.join(HERE, "latency-fail.pcap"), recs)


def v12(outdir):
    """Known latencies, across a 32-bit wrap of the AVTP time, ns and us captures."""
    os.makedirs(outdir, exist_ok=True)
    wrap = 1 << 32
    t0 = 417_000 * wrap - 60_000_000  # 60 ms before a wrap of the low 32 bits
    known = []
    recs = []
    ramp = Ramp(2)
    for i in range(1000):
        lat = 150_000 + i * 1_013 + (i % 7) * 333_333  # 150 us .. ~3.1 ms
        if i == 999:
            lat = -5_000  # one early frame: reported as negative
        ingress = t0 + i * STEP
        payload = b"".join(struct.pack("!I", v) for v in ramp.chunk(i * SPF, SPF))
        fr = A.build_aaf_frame(DST, SRC, SID_2CH, i, ingress + PTO, payload, 2)
        recs.append((ingress + lat, fr))
        known.append((i % 256, lat))
    A.write_pcap(os.path.join(outdir, "known-ns.pcap"), recs, nanosecond=True)
    A.write_pcap(os.path.join(outdir, "known-us.pcap"), recs, nanosecond=False)
    with open(os.path.join(outdir, "known.csv"), "w") as f:
        f.write("index,seq,latency_ns\n")
        for i, (seq, lat) in enumerate(known):
            f.write(f"{i},{seq},{lat}\n")


def ramps():
    gen = os.path.join(KIT, "ramp-gen.py")
    for name, extra in (("ramp-pass.wav", []), ("ramp-fail.wav", ["--drop-frame", "1000"])):
        subprocess.run([sys.executable, "-I", gen, os.path.join(HERE, name), "--channels", "2",
                        "--frames", "4800", "--lead-silence", "64"] + extra,
                       check=True, stdout=subprocess.DEVNULL)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--v12", metavar="DIR", help="write only the V1.2 known-latency captures into DIR")
    a = ap.parse_args()
    if a.v12:
        v12(a.v12)
        return
    write("cyclictest-pass.txt", cyclictest([48, 52, 61, 39]))
    write("cyclictest-fail.txt", cyclictest([48, 137, 61, 39]))
    write("cyclictest-truncated.txt", cyclictest([48, 52, 61, 39], truncated=True))
    write("ptp4l-pass.log", ptp4l_log("pass"))
    write("ptp4l-fail.log", ptp4l_log("fail"))
    write("ptp4l-summary.log", ptp4l_log("summary"))
    write("pmc-slave.txt", pmc_output(True))
    write("pmc-listening.txt", pmc_output(False))
    shaper_dir("pass")
    shaper_dir("fail-rrobin", rrobin=True)
    shaper_dir("fail-nooffload", offloaded=False)
    shaper_dir("fail-notmoving", move=(0, 1))
    captures()
    ramps()
    total = 0
    for root, _d, files in os.walk(HERE):
        for f in files:
            total += os.path.getsize(os.path.join(root, f))
    print(f"make-samples: fixtures in {HERE}, {total} bytes")


if __name__ == "__main__":
    main()
