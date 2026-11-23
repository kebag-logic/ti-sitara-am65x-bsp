#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0
"""counters.py - the stream counters as milan-mediad counts them, read over AECP.

milan-ctrld and milan-mediad (its talker, its AAF listener and its CRF receive
path, on a simulated USB host) run on the DUT side of a veth pair, inside the namespace
run-netns-counters.sh makes. This side plays the talker: ACMP binds STREAM_INPUT
0 to an AAF stream and STREAM_INPUT 1 to a CRF stream it sends, and a
controller reads GET_COUNTERS while the streams carry one fault at a time:

  clean           FRAMES_RX counts observation intervals, nothing else moves;
                  the CRF input locks on its 2 ms grid (MEDIA_LOCKED)
  lost PDU        one sequence_num skipped: SEQ_NUM_MISMATCH once; the CRF
                  grid holds across the gap (two steps), so no unlock
  tu              five PDUs with tu set: TIMESTAMP_UNCERTAIN in one or two
                  intervals
  mr              mr toggled, then held: MEDIA_RESET once; the CRF input
                  unlocks and locks again
  other format    three PDUs in another format (2 channels; 44.1 kHz CRF):
                  UNSUPPORTED_FORMAT
  late            80 ms without sending, then the backlog at once, stamped as
                  scheduled (PTO 50 ms): LATE_TIMESTAMP; the CRF grid holds
  early           three PDUs stamped 300 ms ahead (more than 4 x PTO):
                  EARLY_TIMESTAMP; they are not processed, the grid holds
  interruption    300 ms with nothing sent (the threshold 200 ms):
                  STREAM_INTERRUPTED on both; the CRF input unlocks, then locks
  rebind          UNBIND_RX and BIND_RX of STREAM_INPUT 1: its counters start
                  from zero (Milan v1.2 5.3.8.10)
  talker          STREAM_OUTPUT 0 (Milan v1.2 Table 5.4): one STREAM_START,
                  FRAMES_TX per interval, MEDIA_RESET and TIMESTAMP_UNCERTAIN
                  zero, since it never toggles mr nor sets tu, as its PDUs show

Every reading checks MEDIA_LOCKED = MEDIA_UNLOCKED or MEDIA_UNLOCKED + 1 (Milan
v1.2 Table 5.6). Counts per interval are judged as one or two, since an event
can straddle an interval's end.

Usage (as root in the namespace): counters.py --dut veth0 --peer veth1 --build DIR --entity FILE --tmp DIR
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import signal
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True


def load(name: str):
    spec = importlib.util.spec_from_file_location(name, HERE / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


aecp = load("aecp")
peer = aecp.peer

PTO_NS = 50_000_000             # the streams' presentation offset, and milan-mediad's -o
INTERRUPT_NS = 200_000_000      # milan-mediad's -T
TICK_NS = 500_000               # an AAF PDU of 24 frames every 500 us
CRF_TICKS = 4                   # a CRF PDU every 2 ms (Milan v1.2 Table 7.1: 96 events at 48 kHz)
AAF_FRAMES = 24
CRF_STEP_NS = 2_000_000
SI = ["MEDIA_LOCKED", "MEDIA_UNLOCKED", "STREAM_INTERRUPTED", "SEQ_NUM_MISMATCH", "MEDIA_RESET",
      "TIMESTAMP_UNCERTAIN", "", "", "UNSUPPORTED_FORMAT", "LATE_TIMESTAMP", "EARLY_TIMESTAMP", "FRAMES_RX"]


def tai_ns() -> int:
    return time.clock_gettime_ns(time.CLOCK_TAI)        # gPTP time on a veth without a PHC (gptp_time.h)


class Stream:
    """One stream's header state, and the faults to put in its next PDUs."""

    def __init__(self, stream_id: int, dmac: int):
        self.stream_id, self.dmac = stream_id, dmac.to_bytes(6, "big")
        self.seq = 0
        self.mr = False
        self.skip = 0           # PDUs to leave out, their sequence numbers used
        self.tu = 0             # PDUs to send with tu set
        self.other = 0          # PDUs in another format
        self.early = 0          # PDUs stamped 300 ms ahead
        self.sent = 0


class Talker:
    """The peer's AAF and CRF talker: PDUs on a fixed schedule of gPTP (TAI) time, each stamped schedule + PTO."""

    def __init__(self, ifname: str, aaf: Stream, crf: Stream):
        self.sock = peer.socket.socket(peer.socket.AF_PACKET, peer.socket.SOCK_RAW, 0)
        self.sock.bind((ifname, 0))
        self.src = self.sock.getsockname()[4]
        self.aaf, self.crf = aaf, crf
        self.hold_until = 0     # TAI ns: send nothing before it; then the backlog
        self.drop_until = 0     # TAI ns: send nothing before it, and drop what was due
        self.lock = threading.Lock()
        self.stop = False
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _aaf(self, s: Stream, t: int) -> bytes:
        tu, other, early = s.tu > 0, s.other > 0, s.early > 0
        channels = 2 if other else 8
        stamp = (t + PTO_NS + (300_000_000 if early else 0)) & 0xFFFFFFFF
        h = struct.pack(">BBBBQIBBBBHBB", 0x02, 0x80 | (0x08 if s.mr else 0) | 0x01, s.seq, 0x01 if tu else 0,
                        s.stream_id, stamp, 0x02, (0x5 << 4) | (channels >> 8), channels & 0xFF, 32,
                        AAF_FRAMES * channels * 4, 0, 0)
        return s.dmac + self.src + b"\x22\xf0" + h + bytes(AAF_FRAMES * channels * 4)

    def _crf(self, s: Stream, t: int) -> bytes:
        tu, other, early = s.tu > 0, s.other > 0, s.early > 0
        base = 44100 if other else 48000
        stamp = t + PTO_NS + (300_000_000 if early else 0)
        h = struct.pack(">BBBBQIHHQ", 0x04, 0x80 | (0x08 if s.mr else 0) | (0x01 if tu else 0), s.seq, 0x01,
                        s.stream_id, base & 0x1FFFFFFF, 8, 96, stamp)
        return s.dmac + self.src + b"\x22\xf0" + h

    def _emit(self, s: Stream, build, t: int, dropping: bool) -> None:
        frame = build(s, t)
        for k in ("tu", "other", "early"):
            setattr(s, k, max(0, getattr(s, k) - 1))
        if s.skip > 0:
            s.skip -= 1
        elif not dropping:
            self.sock.send(frame + b"\0" * max(0, 60 - len(frame)))
            s.sent += 1
        s.seq = (s.seq + 1) & 0xFF

    def _run(self) -> None:
        t0 = (tai_ns() // CRF_STEP_NS + 1) * CRF_STEP_NS
        k = 0
        while not self.stop:
            t = t0 + k * TICK_NS
            now = tai_ns()
            if t > now:
                time.sleep((t - now) / 1e9)
            with self.lock:
                if tai_ns() < self.hold_until:
                    continue            # k stays: the backlog goes out at once when the hold ends
                dropping = t < self.drop_until
                self._emit(self.aaf, self._aaf, t, dropping)
                if k % CRF_TICKS == 0:
                    self._emit(self.crf, self._crf, t, dropping)
            k += 1

    def fault(self, faults: list) -> None:
        """(object, attribute, value) each, set together; a callable value is taken at that moment."""
        with self.lock:
            for obj, name, value in faults:
                setattr(obj, name, value() if callable(value) else value)

    def close(self) -> None:
        self.stop = True
        self.thread.join(timeout=1)
        self.sock.close()


def capture_aaf(ifname: str, dut_mac: bytes, secs: float) -> list[bytes]:
    """The AVTP headers of the AAF PDUs the DUT sends for secs, tagged inline or with the tag moved to metadata."""
    sock = peer.socket.socket(peer.socket.AF_PACKET, peer.socket.SOCK_RAW, peer.socket.htons(3))
    sock.bind((ifname, 3))
    sock.settimeout(0.05)
    out = []
    end = time.monotonic() + secs
    while time.monotonic() < end:
        try:
            f = sock.recv(2048)
        except peer.socket.timeout:
            continue
        if f[6:12] != dut_mac:
            continue
        at = 18 if f[12:14] == b"\x81\x00" else 14
        if f[at - 2:at] == b"\x22\xf0" and len(f) > at + 24 and f[at] == 0x02:
            out.append(f[at:at + 24])
    sock.close()
    return out


def read(c, index: int) -> list[int]:
    v, q = aecp.counters(c, aecp.D_STREAM_INPUT, index)
    return q if v == 0xF3F else []


def delta(a: list[int], b: list[int]) -> dict:
    return {SI[k]: b[k] - a[k] for k in range(12) if SI[k] and b[k] != a[k]}


def main() -> int:
    ap = argparse.ArgumentParser()
    for k in ("--dut", "--peer", "--build", "--entity", "--tmp"):
        ap.add_argument(k, required=True)
    a = ap.parse_args()
    g = peer.Grader()
    probe = peer.socket.socket(peer.socket.AF_PACKET, peer.socket.SOCK_RAW, 0)
    probe.bind((a.dut, 0))
    dut_mac = probe.getsockname()[4]
    probe.close()
    eid = peer.mac_int(dut_mac[:3] + b"\xff\xfe" + dut_mac[3:])
    a.dp = os.path.join(a.build, "milan-dp")
    a.shm = f"/milan-dp-ctr-{os.getpid()}"
    media_name = f"/milan-media-ctr-{os.getpid()}"
    gptp_name = f"flexptpd-ctr-{os.getpid()}"
    ptp = aecp.Flexptpd(gptp_name, eid)
    wire = peer.Wire(a.peer, dut_mac)
    c = aecp.Controller(wire, dut_mac, eid, (peer.CONTROLLER,))
    logs = [open(os.path.join(a.tmp, n), "w") for n in ("ctrld-ctr.log", "mediad-ctr.log")]
    ctrld = subprocess.Popen([os.path.join(a.build, "milan-ctrld"), "-i", a.dut, "-e", a.entity, "-g", gptp_name,
                              "-d", a.shm, "-m", media_name, "-N", "none", "-n", "-v"],
                             stdout=logs[0], stderr=logs[0])
    mediad = None
    talker = None
    aaf = Stream(aecp.TALKER_STREAM_AAF, peer.PEER_STREAM_DMAC + 0)
    crf = Stream(aecp.TALKER_STREAM_CRF, peer.PEER_STREAM_DMAC + 1)
    try:
        r = wire.wait(lambda f: peer.is_sub(peer.SUB_ADP, peer.ADP_AVAILABLE)(f), 7.0)
        g.check("ADP: ENTITY_AVAILABLE", r is not None)
        mediad = subprocess.Popen([os.path.join(a.build, "milan-mediad"), "-i", a.dut, "-r", "talker,listener,crf",
                                   "-S", "0", "-P", "0", "-o", str(PTO_NS), "-T", str(INTERRUPT_NS),
                                   "-d", a.shm, "-m", media_name], stdout=logs[1], stderr=logs[1])
        time.sleep(1.0)
        before = [read(c, 0), read(c, 1)]
        g.check("GET_COUNTERS STREAM_INPUT 0 and 1 before any binding: counted (valid 0xf3f), all zero",
                before == [[0] * 12, [0] * 12], str(before))
        talker = Talker(a.peer, aaf, crf)
        aecp.bind(g, c, wire, eid, a, 0, 0, aecp.TALKER_STREAM_AAF)
        aecp.bind(g, c, wire, eid, a, 1, 1, aecp.TALKER_STREAM_CRF)
        time.sleep(3.0)

        def step(name: str, wait: float, want0: dict, want1: dict, faults: list) -> None:
            q0, q1 = read(c, 0), read(c, 1)
            talker.fault(faults)
            time.sleep(wait)
            r0, r1 = read(c, 0), read(c, 1)
            if not g.check(f"{name}: both inputs read", len(q0) == len(q1) == len(r0) == len(r1) == 12):
                return
            for index, (q, r, want) in enumerate(((q0, r0, want0), (q1, r1, want1))):
                d = delta(q, r)
                ok = all(lo <= d.get(k, 0) <= hi for k, (lo, hi) in want.items())
                moved = [f"{k} +{lo}" + (f"..{hi}" if hi != lo else "") for k, (lo, hi) in want.items()
                         if k != "FRAMES_RX"]
                g.check(f"{name}: STREAM_INPUT {index} {', '.join(moved) or 'nothing but FRAMES_RX'}",
                        ok and all(k in want for k in d), str(d))
                g.check(f"{name}: STREAM_INPUT {index} MEDIA_LOCKED = MEDIA_UNLOCKED (+1)",
                        r[0] in (r[1], r[1] + 1), f"{r[0]}/{r[1]}")

        q1 = read(c, 1)
        g.check("clean, 3 s: STREAM_INPUT 1 (CRF) locked once on its grid", len(q1) == 12 and (q1[0], q1[1]) == (1, 0),
                str(q1))
        g.check("clean, 3 s: STREAM_INPUT 0 and 1 FRAMES_RX counts intervals of 500 ms, not PDUs",
                all(4 <= q[11] <= 8 for q in (read(c, 0), q1)), f"{read(c, 0)[11:]} {q1[11:]}")
        frames = {"FRAMES_RX": (2, 4)}
        aaf_lock = {"MEDIA_LOCKED": (0, 9), "MEDIA_UNLOCKED": (0, 9)}  # the AAF servo's own lock, on this host
        step("clean, 1.5 s", 1.5, {**frames, **aaf_lock}, frames, [])
        step("one PDU lost", 1.5, {**frames, **aaf_lock, "SEQ_NUM_MISMATCH": (1, 1)},
             {**frames, "SEQ_NUM_MISMATCH": (1, 1)}, [(aaf, "skip", 1), (crf, "skip", 1)])
        step("tu on five PDUs", 1.5, {**frames, **aaf_lock, "TIMESTAMP_UNCERTAIN": (1, 2)},
             {**frames, "TIMESTAMP_UNCERTAIN": (1, 2)}, [(aaf, "tu", 5), (crf, "tu", 5)])
        # the AAF listener places its stream again on a media clock restart
        aaf_placed = {**aaf_lock, "STREAM_INTERRUPTED": (0, 1)}
        step("mr toggled", 1.5, {**frames, **aaf_placed, "MEDIA_RESET": (1, 1)},
             {**frames, "MEDIA_RESET": (1, 1), "MEDIA_LOCKED": (1, 1), "MEDIA_UNLOCKED": (1, 1)},
             [(aaf, "mr", True), (crf, "mr", True)])
        step("three PDUs of another format", 1.5, {**frames, **aaf_placed, "UNSUPPORTED_FORMAT": (1, 2)},
             {**frames, "UNSUPPORTED_FORMAT": (1, 2)}, [(aaf, "other", 3), (crf, "other", 3)])
        # a late backlog is jumped back on time by the AAF listener: one interruption of its playback
        step("80 ms held, then the backlog", 1.5, {**frames, **aaf_placed, "LATE_TIMESTAMP": (1, 2)},
             {**frames, "LATE_TIMESTAMP": (1, 2)}, [(talker, "hold_until", lambda: tai_ns() + 80_000_000)])
        step("three PDUs stamped 300 ms ahead", 1.5, {**frames, **aaf_placed, "EARLY_TIMESTAMP": (1, 2)},
             {**frames, "EARLY_TIMESTAMP": (1, 2)}, [(aaf, "early", 3), (crf, "early", 3)])
        step("300 ms with nothing sent", 1.8, {**frames, **aaf_lock, "STREAM_INTERRUPTED": (1, 1)},
             {**frames, "STREAM_INTERRUPTED": (1, 1), "MEDIA_LOCKED": (1, 1), "MEDIA_UNLOCKED": (1, 1)},
             [(talker, "drop_until", lambda: tai_ns() + 300_000_000)])
        out = subprocess.run([a.dp, a.shm, media_name], capture_output=True, text=True, check=False).stdout
        x = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
        raw = {k: x.get(f"crf_{k}") for k in ("seq_mismatch", "timestamp_uncertain", "media_restarts",
                                                 "unsupported_format", "late_timestamp", "early_timestamp",
                                                 "stream_interrupted", "grid_breaks")}
        g.check("milan-dp: the CRF path's raw counts (PDUs, not intervals): 1 skip, 5 tu, 1 mr toggle, 3 other, "
                "late ones, 3 early, 1 interruption, no grid break",
                raw["seq_mismatch"] == "1" and raw["timestamp_uncertain"] == "5" and raw["media_restarts"] == "1"
                and raw["unsupported_format"] == "3" and int(raw["late_timestamp"] or 0) >= 1
                and raw["early_timestamp"] == "3" and raw["stream_interrupted"] == "1" and raw["grid_breaks"] == "0",
                str(raw))
        # rebinding: zeroed for the new binding (Milan v1.2 5.3.8.10)
        aecp.unbind(g, wire, eid, 1, 1)
        time.sleep(0.5)
        held = read(c, 1)
        g.check("UNBIND_RX STREAM_INPUT 1: its counters kept (not reset on bound to not bound)",
                len(held) == 12 and held[3] >= 1 and held[11] >= 10, str(held))
        aecp.bind(g, c, wire, eid, a, 1, 1, aecp.TALKER_STREAM_CRF)
        time.sleep(1.2)
        fresh = read(c, 1)
        g.check("BIND_RX STREAM_INPUT 1 again: counted from zero, only FRAMES_RX (and the lock) since",
                len(fresh) == 12 and fresh[2:11] == [0] * 9 and 1 <= fresh[11] <= 3 and fresh[0] - fresh[1] in (0, 1)
                and fresh[0] <= 1, str(fresh))
        g.check("datapath: sink 1 bound twice", "binds:2" in peer.milan_dp(a.dp, a.shm).get("sink1", ""))
        # the talker: its PDUs as sent, and STREAM_OUTPUT 0's counters
        sent = capture_aaf(a.peer, dut_mac, 0.5)
        bits = {(h[1] & 0x08, h[3] & 0x01) for h in sent}
        g.check("talker: AAF PDUs sent, mr never toggled and tu never set", len(sent) > 1000 and bits == {(0, 0)},
                f"{len(sent)} PDUs, (mr, tu) {bits}")
        v, q = aecp.counters(c, aecp.D_STREAM_OUTPUT, 0)
        g.check("GET_COUNTERS STREAM_OUTPUT 0: valid 0x1f; STREAM_START 1, STREAM_STOP 0, MEDIA_RESET 0, "
                "TIMESTAMP_UNCERTAIN 0, FRAMES_TX counting 500 ms intervals",
                v == 0x1F and q[:4] == [1, 0, 0, 0] and 20 <= q[4] <= 80, f"{v} {q[:5]}")
    finally:
        if talker is not None:
            talker.close()
        for p in (mediad, ctrld):
            if p is not None and p.poll() is None:
                p.send_signal(signal.SIGTERM)
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill()
        c.stop = True
        wire.stop = True
        ptp.close()
        for name in (a.shm, media_name):
            try:
                os.unlink("/dev/shm" + name)
            except OSError:
                pass
        for f in logs:
            f.close()
    print(f"counters: {'PASS' if g.failed == 0 else 'FAIL'} ({g.failed} failed)")
    if g.failed:
        for n in ("ctrld-ctr.log", "mediad-ctr.log"):
            print(f"--- {n} ---")
            print(open(os.path.join(a.tmp, n)).read()[-6000:])
    return 1 if g.failed else 0


if __name__ == "__main__":
    sys.exit(main())
