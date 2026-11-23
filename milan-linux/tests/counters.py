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
  early only      1.5 s of PDUs all stamped 300 ms ahead: none is processed,
                  so both inputs are interrupted once (STREAM_INTERRUPTED), as
                  for a gap, though PDUs keep arriving; the CRF input unlocks
  rebind          UNBIND_RX and BIND_RX of STREAM_INPUT 1: its counters start
                  from zero (Milan v1.2 5.3.8.10)
  burst           with milan-ctrld and milan-mediad held, UNBIND_RX, BIND_RX,
                  UNBIND_RX, BIND_RX of STREAM_INPUT 0, UNBIND_RX and BIND_RX
                  of STREAM_INPUT 1, then GET_COUNTERS of both, queued: served
                  in one go, each binding is counted and both inputs read zero
                  at once, before milan-mediad runs again; then they count
                  from zero for the new binding
  rebind at once  BIND_RX of STREAM_INPUT 1 and GET_COUNTERS the moment its
                  response is seen, five times: zero at once
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


def binds(a, sink: int) -> int:
    x = peer.milan_dp(a.dp, a.shm).get(f"sink{sink}", "")
    return int(x.split("binds:")[1].split()[0]) if "binds:" in x else -1


def hold(p: subprocess.Popen) -> None:
    p.send_signal(signal.SIGSTOP)
    os.waitpid(p.pid, os.WUNTRACED)


def get_counters(c, k: int) -> int:
    """GET_COUNTERS of STREAM_INPUT k sent, not waited for: its sequence_id."""
    c.seq = (c.seq + 1) & 0xFFFF
    c.wire.send(c.dut_mac, aecp.aecpdu(aecp.AEM_CMD, c.eid, peer.CONTROLLER, c.seq, aecp.GET_COUNTERS,
                                       struct.pack(">HH", aecp.D_STREAM_INPUT, k)))
    return c.seq


def burst(g, a, c, wire, eid: int, ctrld, mediad, talker, aaf: Stream, crf: Stream) -> None:
    """UNBIND_RX and BIND_RX queued while milan-ctrld is held, so one loop pass serves several of them, and
    GET_COUNTERS after them while milan-mediad is held, so only the bind count can zero the counters: each binding
    zeroes its input's counters (Milan v1.2 5.3.8.10), however fast the sequence."""
    talker.fault([(aaf, "skip", 1), (crf, "skip", 1)])
    time.sleep(1.2)
    old = [read(c, 0), read(c, 1)]
    g.check("burst: before it, both inputs have counted for their binding (SEQ_NUM_MISMATCH, FRAMES_RX)",
            all(len(q) == 12 and q[3] >= 1 and q[11] >= 2 for q in old), str(old))
    before = [binds(a, 0), binds(a, 1)]
    hold(mediad)
    hold(ctrld)
    t = time.monotonic()
    queued = [(peer.UNBIND_RX_CMD, 0, 0x7100), (peer.BIND_RX_CMD, 0, 0x7101), (peer.UNBIND_RX_CMD, 0, 0x7102),
              (peer.BIND_RX_CMD, 0, 0x7103), (peer.UNBIND_RX_CMD, 1, 0x7110), (peer.BIND_RX_CMD, 1, 0x7111)]
    for typ, luid, seq in queued:
        wire.send(peer.ADP_ACMP_MC, peer.acmpdu(typ, controller=peer.CONTROLLER, talker=peer.TALKER_PEER,
                                                listener=eid, tuid=luid, luid=luid, seq=seq))
    asked = {k: get_counters(c, k) for k in (0, 1)}
    time.sleep(0.03)
    ctrld.send_signal(signal.SIGCONT)
    last_bind = {}
    for typ, luid, seq in queued:
        r = wire.wait(lambda f: peer.is_sub(peer.SUB_ACMP, typ + 1)(f) and peer.parse_acmp(f[14:])["seq"] == seq,
                      1.0, t)
        g.check(f"burst: {'BIND_RX' if typ == peer.BIND_RX_CMD else 'UNBIND_RX'} STREAM_INPUT {luid} "
                f"({seq:#x}): SUCCESS", r is not None and peer.parse_acmp(r[1][14:])["status"] == 0)
        if r is not None and typ == peer.BIND_RX_CMD:
            last_bind[luid] = r[0]
    # at once after the last BIND_RX response: before the far side's next look at the media block
    now = {k: get_counters(c, k) for k in (0, 1)}

    def answer(seq: int):
        r = wire.wait(lambda f: aecp.is_aecp(f) and (lambda q: q["msg"] == aecp.AEM_RSP and q["seq"] == seq
                                                      and q["cmd"] == aecp.GET_COUNTERS
                                                      and not q["unsolicited"])(aecp.parse_aecp(f[14:])), 1.0, t)
        q = aecp.parse_aecp(r[1][14:]) if r else None
        if q is None or q["status"] != aecp.SUCCESS or len(q["payload"]) < 136:
            return None, None, []
        return r[0], aecp.u32(q["payload"], 4), [aecp.u32(q["payload"], 8 + 4 * j) for j in range(12)]
    for k in (0, 1):
        # queued behind the bindings on the wire, but AECP may be served first in the same pass: answered after
        # the binding's response, it is the new binding's (zero); before, the old one's
        at, valid, values = answer(asked[k])
        after_bind = at is not None and k in last_bind and at > last_bind[k]
        g.check(f"burst: GET_COUNTERS STREAM_INPUT {k} queued with them: valid 0xf3f; "
                f"{'answered after its BIND_RX response, all zero' if after_bind else 'answered first, the old binding'}"
                "'s", valid == 0xF3F and (values == [0] * 12 if after_bind else values[3] >= 1),
                f"{valid if valid is None else hex(valid)} {values}")
        at, valid, values = answer(now[k])
        g.check(f"burst: GET_COUNTERS STREAM_INPUT {k} at once after the last BIND_RX response, milan-mediad held: "
                "valid 0xf3f, all zero", valid == 0xF3F and values == [0] * 12,
                f"{valid if valid is None else hex(valid)} {values}")
    after = [binds(a, 0), binds(a, 1)]
    g.check("burst: each binding counted, two for STREAM_INPUT 0, one for STREAM_INPUT 1",
            after == [before[0] + 2, before[1] + 1], f"{before} -> {after}")
    # the talker's answers to the probes the bindings sent, so both inputs settle again
    time.sleep(0.1)
    with wire.lock:
        probes = [f for at, f in wire.frames if at >= t and peer.is_sub(peer.SUB_ACMP, peer.PROBE_TX_CMD)(f)]
    for f in probes:
        q = peer.parse_acmp(f[14:])
        stream = aecp.TALKER_STREAM_AAF if q["luid"] == 0 else aecp.TALKER_STREAM_CRF
        wire.send(peer.ADP_ACMP_MC, peer.acmpdu(peer.PROBE_TX_RSP, stream_id=stream, controller=q["controller"],
                                                talker=peer.TALKER_PEER, listener=eid, tuid=q["tuid"],
                                                luid=q["luid"], dmac=peer.PEER_STREAM_DMAC + q["luid"],
                                                seq=q["seq"], vlan=2))
    time.sleep(0.3)
    held = [aecp.counters(c, aecp.D_STREAM_INPUT, k) for k in (0, 1)]
    g.check("burst: GET_COUNTERS again, milan-mediad still held: both valid 0xf3f, all zero",
            held == [(0xF3F, [0] * 12)] * 2, str(held))
    mediad.send_signal(signal.SIGCONT)
    time.sleep(1.2)
    fresh = [read(c, 0), read(c, 1)]
    g.check("burst: milan-mediad running again, both count from zero for the new binding: no SEQ_NUM_MISMATCH, "
            "FRAMES_RX 1 to 3", all(len(q) == 12 and q[2:11] == [0] * 9 and 1 <= q[11] <= 3 for q in fresh),
            str(fresh))
    out = subprocess.run([a.dp, a.shm, a.media], capture_output=True, text=True, check=False).stdout
    served = {k: f"binds:{after[k]}" in next((x for x in out.splitlines()
                                               if x.startswith(f"milan_stream_input{k}=")), "") for k in (0, 1)}
    g.check("burst: milan-mediad zeroed each input for its latest binding (its bind count is the datapath's)",
            served == {0: True, 1: True}, out.replace("\n", " ")[-400:])
    sinks = [peer.milan_dp(a.dp, a.shm).get(f"sink{k}", "") for k in (0, 1)]
    g.check("burst: both inputs listening again", all("listening:1" in x for x in sinks), str(sinks))


def rebind_at_once(g, a, c, wire, eid: int) -> None:
    """BIND_RX of STREAM_INPUT 1, and GET_COUNTERS the moment its response is seen, five times: the new binding
    reads zero at once, though milan-mediad has not yet zeroed for it and the far side last read its block up to
    100 ms before; the previous binding counted FRAMES_RX, which the unbind keeps."""
    for n in range(5):
        time.sleep(1.2)
        aecp.unbind(g, wire, eid, 1, 1)
        time.sleep(0.3)
        kept = read(c, 1)
        t = time.monotonic()
        seq = 0x7200 + n
        wire.send(peer.ADP_ACMP_MC, peer.acmpdu(peer.BIND_RX_CMD, controller=peer.CONTROLLER, talker=peer.TALKER_PEER,
                                                listener=eid, tuid=1, luid=1, seq=seq))
        rsp = wire.wait(lambda f: peer.is_sub(peer.SUB_ACMP, peer.BIND_RX_RSP)(f)
                        and peer.parse_acmp(f[14:])["seq"] == seq, 1.0, t)
        asked = get_counters(c, 1)
        r = wire.wait(lambda f: aecp.is_aecp(f) and (lambda q: q["msg"] == aecp.AEM_RSP and q["seq"] == asked
                                                      and q["cmd"] == aecp.GET_COUNTERS
                                                      and not q["unsolicited"])(aecp.parse_aecp(f[14:])), 1.0, t)
        q = aecp.parse_aecp(r[1][14:]) if r else None
        ok = q is not None and q["status"] == aecp.SUCCESS and len(q["payload"]) >= 136
        valid = aecp.u32(q["payload"], 4) if ok else None
        values = [aecp.u32(q["payload"], 8 + 4 * j) for j in range(12)] if ok else []
        g.check(f"rebind {n + 1} of 5: unbound, STREAM_INPUT 1 kept its FRAMES_RX", len(kept) == 12 and kept[11] >= 1,
                str(kept))
        g.check(f"rebind {n + 1} of 5: GET_COUNTERS STREAM_INPUT 1 at once after the BIND_RX response "
                f"({(r[0] - rsp[0]) * 1000 if r and rsp else -1:.1f} ms): valid 0xf3f, all zero",
                rsp is not None and valid == 0xF3F and values == [0] * 12,
                f"{valid if valid is None else hex(valid)} {values}")
        probe = wire.wait(lambda f: peer.is_sub(peer.SUB_ACMP, peer.PROBE_TX_CMD)(f)
                          and peer.parse_acmp(f[14:])["luid"] == 1, 1.0, t)
        if probe is not None:
            p = peer.parse_acmp(probe[1][14:])
            wire.send(peer.ADP_ACMP_MC, peer.acmpdu(peer.PROBE_TX_RSP, stream_id=aecp.TALKER_STREAM_CRF,
                                                    controller=p["controller"], talker=peer.TALKER_PEER, listener=eid,
                                                    tuid=p["tuid"], luid=p["luid"], dmac=peer.PEER_STREAM_DMAC + 1,
                                                    seq=p["seq"], vlan=2))


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
    a.media = media_name
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
        # PDUs that keep arriving, none of them processed: an interruption, judged on each PDU, not only on a
        # receive that times out (which a stream every 2 ms under a 5 ms timeout may never reach)
        step("1.5 s of PDUs all stamped 300 ms ahead", 1.5,
             {**frames, **aaf_lock, "EARLY_TIMESTAMP": (2, 4), "STREAM_INTERRUPTED": (1, 1)},
             {**frames, "EARLY_TIMESTAMP": (2, 4), "STREAM_INTERRUPTED": (1, 1), "MEDIA_UNLOCKED": (1, 1)},
             [(aaf, "early", 100000), (crf, "early", 100000)])
        talker.fault([(aaf, "early", 0), (crf, "early", 0)])
        time.sleep(1.0)
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
        # last: holding milan-mediad holds its talker too
        burst(g, a, c, wire, eid, ctrld, mediad, talker, aaf, crf)
        rebind_at_once(g, a, c, wire, eid)
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
