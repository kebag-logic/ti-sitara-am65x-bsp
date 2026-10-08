#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0
"""peer.py - drive milan-ctrld from the other end of a veth pair.

Plays every role the bench plays for issue #8 (A1), on one host, inside the
network namespace run-netns.sh makes:

  ptp4l        a fake read-only management socket answering TIME_STATUS_NP and
               PORT_DATA_SET_NP with a grandmaster the test chooses
  controller   ENTITY_DISCOVER, BIND_RX, UNBIND_RX
  listener     PROBE_TX_COMMAND to the PB2's talker
  talker       PROBE_TX_RESPONSE to the PB2's listener
  MAAP peer    a conflicting PROBE the PB2 must DEFEND against

and grades what milan-ctrld puts on the wire against IEEE 1722.1-2021, IEEE
1722-2016 Annex B and Milan v1.2, plus what it publishes in the datapath
block (read with milan-dp). Exit 0 when every check passes.

Usage (as root in the namespace): peer.py --dut veth0 --peer veth1 --ctrld BIN --dp BIN --entity FILE --tmp DIR
"""

from __future__ import annotations

import argparse
import os
import signal
import socket
import struct
import subprocess
import sys
import threading
import time

ETH_P_ALL = 3
AVTP = 0x22F0
ADP_ACMP_MC = bytes.fromhex("91e0f0010000")
MAAP_MC = bytes.fromhex("91e0f000ff00")
SUB_ADP, SUB_ACMP, SUB_MAAP = 0xFA, 0xFC, 0xFE
ADP_AVAILABLE, ADP_DEPARTING, ADP_DISCOVER = 0, 1, 2
PROBE_TX_CMD, PROBE_TX_RSP, BIND_RX_CMD, BIND_RX_RSP, UNBIND_RX_CMD, UNBIND_RX_RSP = 0, 1, 6, 7, 8, 9
MAAP_PROBE, MAAP_DEFEND, MAAP_ANNOUNCE = 1, 2, 3
MAAP_POOL_BASE, MAAP_POOL_SIZE = 0x91E0F0000000, 0xFE00

GM_A = 0x3CC0C6FFFE0A0001
GM_B = 0x3CC0C6FFFE0B0002
CONTROLLER = 0x0200C0FFEE000001
TALKER_PEER = 0x020000FFFE000002        # the peer's entity, when it plays a talker
LISTENER_PEER = 0x020000FFFE000003      # ... and a listener
PEER_STREAM = 0x0200000000020000
PEER_STREAM_DMAC = 0x91E0F000AB00


def mac_int(b: bytes) -> int:
    return int.from_bytes(b, "big")


# ---- frames -------------------------------------------------------------------

def eth(dst: bytes, src: bytes, payload: bytes) -> bytes:
    f = dst + src + struct.pack(">H", AVTP) + payload
    return f + b"\0" * max(0, 60 - len(f))


def adpdu(msg: int, entity_id: int) -> bytes:
    # subtype, sv/version/message_type, valid_time(5)|control_data_length(11), entity_id, then zeros
    return struct.pack(">BBHQ", SUB_ADP, msg, (10 << 11) | 56, entity_id) + b"\0" * 56


def parse_adp(p: bytes) -> dict:
    f = {}
    f["msg"] = p[1] & 0x0F
    f["valid_time"] = p[2] >> 3
    f["cdl"] = struct.unpack(">H", p[2:4])[0] & 0x7FF
    (f["entity_id"], f["entity_model_id"], f["entity_capabilities"], f["talker_stream_sources"],
     f["talker_capabilities"], f["listener_stream_sinks"], f["listener_capabilities"],
     f["controller_capabilities"], f["available_index"], f["gm"], f["domain"]) = struct.unpack(
        ">QQIHHHHIIQB", p[4:49])
    f["identify_control_index"] = struct.unpack(">H", p[52:54])[0]
    return f


def acmpdu(msg: int, status: int = 0, stream_id: int = 0, controller: int = 0, talker: int = 0,
           listener: int = 0, tuid: int = 0, luid: int = 0, dmac: int = 0, count: int = 0, seq: int = 0,
           flags: int = 0, vlan: int = 0) -> bytes:
    return (struct.pack(">BBHQQQQHH", SUB_ACMP, msg, (status << 11) | 44, stream_id, controller, talker,
                        listener, tuid, luid)
            + dmac.to_bytes(6, "big") + struct.pack(">HHHHH", count, seq, flags, vlan, 0))


def parse_acmp(p: bytes) -> dict:
    f = {"msg": p[1] & 0x0F, "status": p[2] >> 3, "cdl": struct.unpack(">H", p[2:4])[0] & 0x7FF}
    (f["stream_id"], f["controller"], f["talker"], f["listener"], f["tuid"], f["luid"]) = struct.unpack(
        ">QQQQHH", p[4:40])
    f["dmac"] = int.from_bytes(p[40:46], "big")
    f["count"], f["seq"], f["flags"], f["vlan"] = struct.unpack(">HHHH", p[46:54])
    return f


def maap_pdu(msg: int, start: int, count: int, cstart: int = 0, ccount: int = 0) -> bytes:
    return (struct.pack(">BBHQ", SUB_MAAP, msg, (1 << 11) | 16, 0) + start.to_bytes(6, "big")
            + struct.pack(">H", count) + cstart.to_bytes(6, "big") + struct.pack(">H", ccount))


def parse_maap(p: bytes) -> dict:
    return {"msg": p[1] & 0x0F, "start": int.from_bytes(p[12:18], "big"),
            "count": struct.unpack(">H", p[18:20])[0], "cstart": int.from_bytes(p[20:26], "big"),
            "ccount": struct.unpack(">H", p[26:28])[0]}


# ---- the wire -------------------------------------------------------------------

class Wire:
    """Every AVTP frame the DUT sends, timestamped, and a way to send ours."""

    def __init__(self, ifname: str, dut_mac: bytes):
        self.sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
        self.sock.bind((ifname, ETH_P_ALL))
        self.src = self.sock.getsockname()[4]
        self.dut_mac = dut_mac
        self.frames: list[tuple[float, bytes]] = []
        self.lock = threading.Lock()
        self.stop = False
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self) -> None:
        self.sock.settimeout(0.1)
        while not self.stop:
            try:
                data, addr = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            if addr[2] == socket.PACKET_OUTGOING or len(data) < 16:
                continue
            if data[6:12] != self.dut_mac or struct.unpack(">H", data[12:14])[0] != AVTP:
                continue
            with self.lock:
                self.frames.append((time.monotonic(), data))

    def send(self, dst: bytes, payload: bytes) -> None:
        self.sock.send(eth(dst, self.src, payload))

    def wait(self, pred, timeout: float, since: float = 0.0):
        """The first (t, frame) after `since` with pred(frame) true, or None."""
        end = time.monotonic() + timeout
        seen = 0
        while time.monotonic() < end:
            with self.lock:
                frames = self.frames[seen:]
                seen = len(self.frames)
            for t, f in frames:
                if t >= since and pred(f):
                    return t, f
            time.sleep(0.01)
        return None

    def all(self, pred, since: float = 0.0) -> list[tuple[float, bytes]]:
        with self.lock:
            return [(t, f) for t, f in self.frames if t >= since and pred(f)]


def is_sub(sub: int, msg: int | None = None):
    return lambda f: f[14] == sub and (msg is None or (f[15] & 0x0F) == msg)


# ---- the fake ptp4l ---------------------------------------------------------------

class FakePtp4l:
    """ptp4l's read-only management socket, answering two GETs."""

    def __init__(self, path: str):
        self.gm = GM_A
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        if os.path.exists(path):
            os.unlink(path)
        self.sock.bind(path)
        self.sock.settimeout(0.1)
        self.gets = 0
        self.stop = False
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self) -> None:
        while not self.stop:
            try:
                req, addr = self.sock.recvfrom(512)
            except socket.timeout:
                continue
            if len(req) < 54 or (req[0] & 0x0F) != 0x0D or (req[0] >> 4) != 1:
                continue
            self.gets += 1
            mid = struct.unpack(">H", req[52:54])[0]
            if mid == 0xC000:
                data = struct.pack(">qqiiH", 0, 0, 0, 0, 0) + b"\0" * 12 + struct.pack(">iQ", 1, self.gm)
            elif mid == 0xC002:
                data = struct.pack(">Ii", 800, 1)
            else:
                continue
            rsp = bytearray(req[:54])
            rsp[46] = (rsp[46] & 0xF0) | 2
            struct.pack_into(">H", rsp, 2, 54 + len(data))
            struct.pack_into(">H", rsp, 50, 2 + len(data))
            self.sock.sendto(bytes(rsp) + data, addr)


# ---- the test ---------------------------------------------------------------------

class Grader:
    def __init__(self):
        self.failed = 0

    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        print(f"[{'PASS' if ok else 'FAIL'}] {name}{': ' + detail if detail else ''}", flush=True)
        self.failed += 0 if ok else 1
        return ok


def read_conf(path: str) -> dict:
    out = {}
    for line in open(path):
        line = line.split("#", 1)[0].strip()
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = int(v.strip(), 0)
    return out


def milan_dp(binary: str, name: str) -> dict:
    out = subprocess.run([binary, name], capture_output=True, text=True, check=False).stdout
    return dict(line.split("=", 1) for line in out.splitlines() if "=" in line)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dut", required=True)
    ap.add_argument("--peer", required=True)
    ap.add_argument("--ctrld", required=True)
    ap.add_argument("--dp", required=True)
    ap.add_argument("--entity", required=True)
    ap.add_argument("--tmp", required=True)
    a = ap.parse_args()

    g = Grader()
    conf = read_conf(a.entity)
    # /sys/class/net is the host's inside `unshare -rn`, so ask a socket
    probe = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, 0)
    probe.bind((a.dut, 0))
    dut_mac = probe.getsockname()[4]
    probe.close()
    eid = mac_int(dut_mac[:3] + b"\xff\xfe" + dut_mac[3:])
    shm = f"/milan-dp-test-{os.getpid()}"
    ptp = FakePtp4l(os.path.join(a.tmp, "ptp4lro"))
    wire = Wire(a.peer, dut_mac)

    log = open(os.path.join(a.tmp, "ctrld.log"), "w")
    t0 = time.monotonic()
    proc = subprocess.Popen([a.ctrld, "-i", a.dut, "-e", a.entity, "-p", os.path.join(a.tmp, "ptp4lro"),
                             "-l", os.path.join(a.tmp, "ctrld.ptp"), "-d", shm,
                             "-N", os.path.join(a.tmp, "journal.bin"), "-v"], stdout=log, stderr=log)
    try:
        # A1.2: ENTITY_AVAILABLE with the entity's identity, shape and grandmaster
        r = wire.wait(lambda f: is_sub(SUB_ADP, ADP_AVAILABLE)(f) and parse_adp(f[14:])["gm"] == GM_A, 7.0)
        if g.check("ADP: ENTITY_AVAILABLE carrying ptp4l's grandmaster", r is not None,
                   f"after {r[0] - t0:.2f} s" if r else "none in 7 s"):
            p = parse_adp(r[1][14:])
            g.check("ADP: destination 91:e0:f0:01:00:00, 82-byte frame", r[1][:6] == ADP_ACMP_MC and len(r[1]) >= 82)
            g.check("ADP: entity_id is the MAC's EUI-64", p["entity_id"] == eid, f"{p['entity_id']:016x}")
            g.check("ADP: control_data_length 56, valid_time 10", p["cdl"] == 56 and p["valid_time"] == 10)
            want = {k: conf[k] for k in ("entity_model_id", "entity_capabilities", "talker_stream_sources",
                                         "talker_capabilities", "listener_stream_sinks",
                                         "listener_capabilities", "identify_control_index")}
            got = {k: p[k] for k in want}
            g.check("ADP: fields as entity.conf", got == want, "" if got == want else f"{got} != {want}")
            g.check("ADP: gPTP domain 0", p["domain"] == 0)
        g.check("ptp4l: management GETs received", ptp.gets > 0, f"{ptp.gets}")

        # the datapath block carries the identity and the grandmaster
        dp = milan_dp(a.dp, shm)
        g.check("datapath: entity_id and grandmaster published",
                dp.get("entity_id") == f"{eid:016x}" and dp.get("gm_id") == f"{GM_A:016x}", str(dp.get("gm_id")))

        # A1.4: MAAP probes then announces a range for the talker's sources
        ann = wire.wait(is_sub(SUB_MAAP, MAAP_ANNOUNCE), 6.0)
        probes = wire.all(is_sub(SUB_MAAP, MAAP_PROBE))
        if g.check("MAAP: ANNOUNCE after its PROBEs", ann is not None and len(probes) >= 3,
                   f"{len(probes)} PROBEs"):
            m = parse_maap(ann[1][14:])
            gaps = [round((probes[i + 1][0] - probes[i][0]) * 1000) for i in range(len(probes) - 1)]
            g.check("MAAP: PROBE spacing within 500 to 600 ms (+/-20 ms host jitter)",
                    all(480 <= x <= 620 for x in gaps), f"{gaps} ms")
            g.check("MAAP: range inside the pool, one address per talker source",
                    MAAP_POOL_BASE <= m["start"] and m["start"] + m["count"] <= MAAP_POOL_BASE + MAAP_POOL_SIZE
                    and m["count"] == conf["talker_stream_sources"], f"{m['start']:012x} x {m['count']}")
            g.check("MAAP: sent to 91:e0:f0:00:ff:00", ann[1][:6] == MAAP_MC)
            dp = milan_dp(a.dp, shm)
            g.check("datapath: MAAP range and source 0's address",
                    dp.get("maap_valid") == "1" and dp.get("maap_base") == f"{m['start']:012x}"
                    and f"dest_mac:{m['start']:012x}" in dp.get("source0", ""), str(dp.get("source0")))

            # A1.4: a conflicting PROBE for our range is DEFENDed
            t = time.monotonic()
            wire.send(MAAP_MC, maap_pdu(MAAP_PROBE, m["start"], 1))
            d = wire.wait(is_sub(SUB_MAAP, MAAP_DEFEND), 1.0, t)
            g.check("MAAP: DEFEND against a conflicting PROBE", d is not None,
                    f"after {(d[0] - t) * 1000:.0f} ms" if d else "none in 1 s")

            # A1.5: the talker answers PROBE_TX with its stream
            t = time.monotonic()
            wire.send(ADP_ACMP_MC, acmpdu(PROBE_TX_CMD, controller=CONTROLLER, talker=eid,
                                          listener=LISTENER_PEER, tuid=0, luid=5, seq=0x4242))
            r = wire.wait(lambda f: is_sub(SUB_ACMP, PROBE_TX_RSP)(f) and parse_acmp(f[14:])["seq"] == 0x4242,
                          1.0, t)
            if g.check("ACMP talker: PROBE_TX_RESPONSE", r is not None,
                       f"after {(r[0] - t) * 1000:.0f} ms" if r else "none in 1 s"):
                q = parse_acmp(r[1][14:])
                g.check("ACMP talker: SUCCESS with the stream, MAAP address and VLAN 2",
                        q["status"] == 0 and q["stream_id"] == (mac_int(dut_mac) << 16) and q["dmac"] == m["start"]
                        and q["vlan"] == 2 and q["luid"] == 5 and q["listener"] == LISTENER_PEER,
                        f"status {q['status']} stream {q['stream_id']:016x} dmac {q['dmac']:012x} vlan {q['vlan']}")
                g.check("ACMP talker: response within 200 ms (Milan Table 5.26)", r[0] - t < 0.2)

        # an unknown source is refused
        t = time.monotonic()
        wire.send(ADP_ACMP_MC, acmpdu(PROBE_TX_CMD, controller=CONTROLLER, talker=eid, listener=LISTENER_PEER,
                                      tuid=9, seq=0x4343))
        r = wire.wait(lambda f: is_sub(SUB_ACMP, PROBE_TX_RSP)(f) and parse_acmp(f[14:])["seq"] == 0x4343, 1.0, t)
        g.check("ACMP talker: TALKER_UNKNOWN_ID for source 9", r is not None and parse_acmp(r[1][14:])["status"] == 2)

        # the listener: BIND_RX, its probe of the talker, settlement
        t = time.monotonic()
        wire.send(ADP_ACMP_MC, acmpdu(BIND_RX_CMD, controller=CONTROLLER, talker=TALKER_PEER, listener=eid,
                                      tuid=0, luid=0, seq=0x5151))
        rsp = wire.wait(lambda f: is_sub(SUB_ACMP, BIND_RX_RSP)(f) and parse_acmp(f[14:])["seq"] == 0x5151, 1.0, t)
        g.check("ACMP listener: BIND_RX_RESPONSE SUCCESS", rsp is not None and parse_acmp(rsp[1][14:])["status"] == 0)
        probe = wire.wait(lambda f: is_sub(SUB_ACMP, PROBE_TX_CMD)(f) and parse_acmp(f[14:])["talker"] == TALKER_PEER,
                          1.0, t)
        if g.check("ACMP listener: PROBE_TX_COMMAND to the bound talker", probe is not None):
            q = parse_acmp(probe[1][14:])
            wire.send(ADP_ACMP_MC, acmpdu(PROBE_TX_RSP, stream_id=PEER_STREAM, controller=q["controller"],
                                          talker=TALKER_PEER, listener=eid, tuid=q["tuid"], luid=q["luid"],
                                          dmac=PEER_STREAM_DMAC, seq=q["seq"], vlan=2))
            ok = False
            for _ in range(50):
                sink = milan_dp(a.dp, shm).get("sink0", "")
                if f"stream_id:{PEER_STREAM:016x}" in sink and "listening:1" in sink:
                    ok = True
                    break
                time.sleep(0.02)
            g.check("datapath: sink 0 listening to the talker's stream", ok, sink)

        t = time.monotonic()
        wire.send(ADP_ACMP_MC, acmpdu(UNBIND_RX_CMD, controller=CONTROLLER, talker=TALKER_PEER, listener=eid,
                                      tuid=0, luid=0, seq=0x5252))
        rsp = wire.wait(lambda f: is_sub(SUB_ACMP, UNBIND_RX_RSP)(f) and parse_acmp(f[14:])["seq"] == 0x5252, 1.0, t)
        g.check("ACMP listener: UNBIND_RX_RESPONSE SUCCESS", rsp is not None and parse_acmp(rsp[1][14:])["status"] == 0)
        time.sleep(0.1)
        g.check("datapath: sink 0 stopped", "listening:0" in milan_dp(a.dp, shm).get("sink0", ""))

        # ENTITY_DISCOVER is answered within Milan's 0-4 s random delay
        t = time.monotonic()
        wire.send(ADP_ACMP_MC, adpdu(ADP_DISCOVER, 0))
        r = wire.wait(is_sub(SUB_ADP, ADP_AVAILABLE), 4.5, t)
        g.check("ADP: AVAILABLE after a global DISCOVER (<= 4 s + 0.5 s)", r is not None,
                f"after {r[0] - t:.2f} s" if r else "none")

        # A1.3: a grandmaster change reaches ADP
        t = time.monotonic()
        ptp.gm = GM_B
        r = wire.wait(lambda f: is_sub(SUB_ADP, ADP_AVAILABLE)(f) and parse_adp(f[14:])["gm"] == GM_B, 5.5, t)
        g.check("ADP: AVAILABLE with the new grandmaster (<= poll + 4 s + 1 s)", r is not None,
                f"after {r[0] - t:.2f} s" if r else "none")

        # departing
        t = time.monotonic()
        proc.send_signal(signal.SIGTERM)
        r = wire.wait(is_sub(SUB_ADP, ADP_DEPARTING), 1.0, t)
        g.check("ADP: ENTITY_DEPARTING on SIGTERM", r is not None,
                f"after {(r[0] - t) * 1000:.0f} ms" if r else "none")
        rc = proc.wait(timeout=3)
        g.check("milan-ctrld exits 0", rc == 0, f"rc {rc}")
    finally:
        if proc.poll() is None:
            proc.kill()
        wire.stop = True
        ptp.stop = True
        try:
            os.unlink("/dev/shm" + shm)
        except OSError:
            pass
        log.close()
    print(f"peer: {'PASS' if g.failed == 0 else 'FAIL'} ({g.failed} failed)")
    if g.failed:
        print("--- milan-ctrld log ---")
        print(open(os.path.join(a.tmp, "ctrld.log")).read())
    return 1 if g.failed else 0


if __name__ == "__main__":
    sys.exit(main())
