#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0
"""aecp.py - enumerate milan-ctrld's entity over AECP from the other end of a veth pair.

A controller on the peer side of the pair, inside the network namespace
run-netns-aecp.sh makes, enumerates the entity the way a Milan controller does
and grades every answer against IEEE 1722.1-2021 clauses 7 and 9 and Milan
v1.2 section 5.4:

  READ_DESCRIPTOR  ENTITY, then CONFIGURATION 0, then every descriptor its
                   descriptor_counts list, each answered with its own type and
                   index, and the ones the board's inventory is about read
                   field by field: the pinned entity_model_id, this board's
                   run-time identity in ENTITY and AVB_INTERFACE, STREAM_INPUT 0
                   AAF 48 kHz 8 ch 32-bit, STREAM_INPUT 1 CRF, STREAM_OUTPUT 0
                   AAF, the CRF CLOCK_SOURCE located on STREAM_INPUT 1
  Milan MVU        GET_MILAN_INFO, SET and GET_SYSTEM_UNIQUE_ID
  ownership        ACQUIRE_ENTITY (NOT_SUPPORTED, Milan v1.2 5.4.2.1), LOCK_ENTITY
                   and a second controller refused while it holds, ENTITY_AVAILABLE
  the 5.4 set      the commands milan-fpga's AECP implements (aecp_commands.c),
                   with what the board answers through the face (aecp_far.h):
                   GET/SET_CONFIGURATION, GET/SET_STREAM_FORMAT,
                   GET_STREAM_INFO, GET/SET_NAME, GET/SET_SAMPLING_RATE,
                   GET/SET_CLOCK_SOURCE, GET/SET_CONTROL (IDENTIFY),
                   START/STOP_STREAMING, GET_AVB_INFO, GET_AS_PATH, GET_COUNTERS
                   (the media block's counters), GET_AUDIO_MAP, ADD_AUDIO_MAPPINGS,
                   GET_DYNAMIC_INFO, REBOOT, REGISTER/DEREGISTER_UNSOLICITED_
                   NOTIFICATION and an unsolicited notification
  ACMP             BIND_RX (CONNECT_RX_COMMAND) on STREAM_INPUT 0 and on
                   STREAM_INPUT 1, each probing its talker and settling, seen in
                   the datapath block and in GET_STREAM_INFO, then UNBIND_RX

It plays flexptpd (a status block with a grandmaster one step away) and
milan-mediad (a media block with known counters), as peer.py does.

Usage (as root in the namespace): aecp.py --dut veth0 --peer veth1 --ctrld BIN --dp BIN --entity FILE --tmp DIR
"""

from __future__ import annotations

import argparse
import ctypes
import importlib.util
import mmap
import os
import signal
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True     # peer.py is loaded from the tree; nothing is cached beside it
_spec = importlib.util.spec_from_file_location("peer", HERE / "peer.py")
peer = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(peer)

SUB_AECP = 0xFB
AEM_CMD, AEM_RSP, MVU_CMD, MVU_RSP = 0, 1, 6, 7
MILAN_PROTOCOL = bytes.fromhex("001bc50ac100")
# IEEE 1722.1-2021 Table 7-125 (command codes) and Table 7-126 (status codes)
ACQUIRE, LOCK, ENTITY_AVAILABLE, CONTROLLER_AVAILABLE, READ_DESCRIPTOR = 0x00, 0x01, 0x02, 0x03, 0x04
SET_CONFIGURATION, GET_CONFIGURATION, SET_STREAM_FORMAT, GET_STREAM_FORMAT = 0x06, 0x07, 0x08, 0x09
GET_STREAM_INFO, SET_NAME, GET_NAME = 0x0F, 0x10, 0x11
SET_SAMPLING_RATE, GET_SAMPLING_RATE, SET_CLOCK_SOURCE, GET_CLOCK_SOURCE = 0x14, 0x15, 0x16, 0x17
SET_CONTROL, GET_CONTROL, START_STREAMING, STOP_STREAMING = 0x18, 0x19, 0x22, 0x23
REGISTER_UNSOL, DEREGISTER_UNSOL = 0x24, 0x25
GET_AVB_INFO, GET_AS_PATH, GET_COUNTERS, REBOOT = 0x27, 0x28, 0x29, 0x2A
GET_AUDIO_MAP, ADD_AUDIO_MAPPINGS, REMOVE_AUDIO_MAPPINGS, GET_DYNAMIC_INFO = 0x2B, 0x2C, 0x2D, 0x4B
SUCCESS, NOT_IMPLEMENTED, NO_SUCH_DESCRIPTOR, ENTITY_LOCKED, BAD_ARGUMENTS, NOT_SUPPORTED = 0, 1, 2, 3, 7, 11
# IEEE 1722.1-2021 Table 7-1
D_ENTITY, D_CONFIGURATION, D_AUDIO_UNIT, D_STREAM_INPUT, D_STREAM_OUTPUT = 0x00, 0x01, 0x02, 0x05, 0x06
D_AVB_INTERFACE, D_CLOCK_SOURCE, D_CONTROL, D_STREAM_PORT_INPUT, D_STREAM_PORT_OUTPUT = 0x09, 0x0A, 0x1A, 0x0E, 0x0F
D_CLOCK_DOMAIN = 0x24

MODEL_ID = 0x001BC50AC1000100
AAF_8CH_48K = None          # read from the descriptor, then checked field by field
CRF_48K = 0x041060010000BB80                    # Milan v1.2 7.3.2 Table 7.1
CONTROLLER_B = 0x0200C0FFEE000002
TALKER_STREAM_AAF = 0x0200000000020000
TALKER_STREAM_CRF = 0x0200000000020001
GM = 0x3CC0C6FFFE0A0001


# ---- AECPDUs ------------------------------------------------------------------------

def aecpdu(msg: int, target: int, controller: int, seq: int, cmd: int, payload: bytes = b"",
           status: int = 0) -> bytes:
    body = struct.pack(">QQHH", target, controller, seq, cmd) + payload
    return struct.pack(">BBH", SUB_AECP, msg, (status << 11) | (len(body) - 8)) + body


def mvupdu(target: int, controller: int, seq: int, cmd: int, payload: bytes = b"") -> bytes:
    body = struct.pack(">QQH", target, controller, seq) + MILAN_PROTOCOL + struct.pack(">H", cmd) + payload
    return struct.pack(">BBH", SUB_AECP, MVU_CMD, len(body) - 8) + body


def parse_aecp(p: bytes) -> dict:
    cdl = struct.unpack(">H", p[2:4])[0] & 0x7FF
    f = {"msg": p[1] & 0x0F, "status": p[2] >> 3, "cdl": cdl}
    f["target"], f["controller"], f["seq"] = struct.unpack(">QQH", p[4:22])
    if f["msg"] in (MVU_CMD, MVU_RSP):
        f["protocol"] = p[22:28]
        f["cmd"] = struct.unpack(">H", p[28:30])[0] & 0x7FFF
        f["payload"] = p[30:12 + cdl]
    else:
        word = struct.unpack(">H", p[22:24])[0]
        f["unsolicited"] = bool(word & 0x8000)
        f["cmd"] = word & 0x7FFF
        f["payload"] = p[24:12 + cdl]
    return f


def is_aecp(f: bytes) -> bool:
    return len(f) >= 38 and f[14] == SUB_AECP     # 14 + the 24-octet AEM header, unpadded on veth


def cstr(b: bytes) -> str:
    return b.split(b"\0", 1)[0].decode("utf-8", "replace")


class Controller:
    """An AECP controller on the peer side: sends commands, waits for their responses, answers the entity's
    CONTROLLER_AVAILABLE probes (Milan v1.2 5.4.5.3) for each controller id it plays."""

    def __init__(self, wire: peer.Wire, dut_mac: bytes, eid: int, ids: tuple[int, ...]):
        self.wire, self.dut_mac, self.eid, self.ids = wire, dut_mac, eid, ids
        self.seq = 0x100
        self.stop = False
        self.probes_answered = 0
        threading.Thread(target=self._probes, daemon=True).start()

    def _probes(self) -> None:
        seen = 0
        while not self.stop:
            with self.wire.lock:
                frames = self.wire.frames[seen:]
                seen = len(self.wire.frames)
            for _, f in frames:
                if not is_aecp(f):
                    continue
                q = parse_aecp(f[14:])
                if q["msg"] == AEM_CMD and q["cmd"] == CONTROLLER_AVAILABLE and q["target"] in self.ids:
                    self.wire.send(self.dut_mac, aecpdu(AEM_RSP, q["target"], self.eid, q["seq"],
                                                        CONTROLLER_AVAILABLE))
                    self.probes_answered += 1
            time.sleep(0.01)

    def command(self, cmd: int, payload: bytes = b"", controller: int = peer.CONTROLLER, timeout: float = 1.0,
                mvu: bool = False) -> tuple[dict | None, float]:
        self.seq = (self.seq + 1) & 0xFFFF
        seq = self.seq
        t = time.monotonic()
        pdu = (mvupdu(self.eid, controller, seq, cmd, payload) if mvu
               else aecpdu(AEM_CMD, self.eid, controller, seq, cmd, payload))
        self.wire.send(self.dut_mac, pdu)
        want = MVU_RSP if mvu else AEM_RSP

        def match(f: bytes) -> bool:
            if not is_aecp(f):
                return False
            q = parse_aecp(f[14:])
            return (q["msg"] == want and q["seq"] == seq and q["controller"] == controller and q["cmd"] == cmd
                    and not q.get("unsolicited", False))
        r = self.wire.wait(match, timeout, t)
        return (parse_aecp(r[1][14:]) if r else None), ((r[0] - t) if r else timeout)

    def read(self, dtype: int, index: int, configuration: int = 0) -> dict | None:
        r, _ = self.command(READ_DESCRIPTOR, struct.pack(">HHHH", configuration, 0, dtype, index))
        return r


# ---- milan-mediad's block, as media.h lays it out ----------------------------------------

class Talker(ctypes.Structure):
    _fields_ = [("active", ctypes.c_uint8), ("locked", ctypes.c_uint8), ("pad", ctypes.c_uint8 * 2),
                ("pitch", ctypes.c_uint32), ("stream_id", ctypes.c_uint64), ("dest_mac", ctypes.c_uint64),
                ("t0", ctypes.c_uint64), ("frames_tx", ctypes.c_uint64), ("stream_starts", ctypes.c_uint64),
                ("stream_stops", ctypes.c_uint64), ("underruns", ctypes.c_uint64), ("overruns", ctypes.c_uint64),
                ("late", ctypes.c_uint64), ("send_errors", ctypes.c_uint64), ("level_target", ctypes.c_int32),
                ("level_min", ctypes.c_int32), ("level_max", ctypes.c_int32), ("level_avg_milli", ctypes.c_int32),
                ("late_max_ns", ctypes.c_int32), ("pto_ns", ctypes.c_uint32)]


class Listener(ctypes.Structure):
    _fields_ = [("active", ctypes.c_uint8), ("locked", ctypes.c_uint8), ("pad", ctypes.c_uint8 * 2),
                ("pitch", ctypes.c_uint32), ("stream_id", ctypes.c_uint64), ("frames_rx", ctypes.c_uint64),
                ("seq_mismatch", ctypes.c_uint64), ("late_timestamp", ctypes.c_uint64),
                ("early_timestamp", ctypes.c_uint64), ("unsupported_format", ctypes.c_uint64),
                ("media_locked", ctypes.c_uint64), ("media_unlocked", ctypes.c_uint64),
                ("media_resets", ctypes.c_uint64), ("stream_interrupted", ctypes.c_uint64),
                ("underruns", ctypes.c_uint64), ("align_min_ns", ctypes.c_int32), ("align_max_ns", ctypes.c_int32),
                ("align_avg_ns", ctypes.c_int32), ("margin_min_ns", ctypes.c_int32), ("in_flight_ns", ctypes.c_int64)]


class Media(ctypes.Structure):
    _fields_ = [("magic", ctypes.c_uint32), ("version", ctypes.c_uint32), ("seq", ctypes.c_uint32),
                ("writer_pid", ctypes.c_uint32), ("gptp_rate_ppb", ctypes.c_int64),
                ("gptp_residual_ns", ctypes.c_int64), ("gptp_calibrated", ctypes.c_uint8),
                ("pad", ctypes.c_uint8 * 7), ("talker", Talker), ("listener", Listener)]


class FakeMedia:
    """milan-mediad's media block (MED1, version 2) with chosen Milan counters."""

    def __init__(self, name: str):
        self.path = "/dev/shm" + name
        size = ctypes.sizeof(Media)
        with open(self.path, "wb") as f:
            f.write(b"\0" * size)
        self.f = open(self.path, "r+b")
        self.map = mmap.mmap(self.f.fileno(), size)
        self.m = Media.from_buffer(self.map)
        self.m.magic, self.m.version, self.m.writer_pid = 0x3144454D, 2, os.getpid()
        self.set(listener=dict(frames_rx=8000, seq_mismatch=2, late_timestamp=3, early_timestamp=4,
                               unsupported_format=5, media_locked=1, media_unlocked=6, media_resets=7,
                               stream_interrupted=8),
                 talker=dict(frames_tx=16000, stream_starts=1, stream_stops=9))

    def set(self, listener: dict | None = None, talker: dict | None = None) -> None:
        self.m.seq += 1
        for k, v in (listener or {}).items():
            setattr(self.m.listener, k, v)
        for k, v in (talker or {}).items():
            setattr(self.m.talker, k, v)
        self.m.seq += 1

    def close(self) -> None:
        del self.m
        self.map.close()
        self.f.close()
        os.unlink(self.path)


class Flexptpd(peer.FakeFlexptpd):
    """peer.py's status block, its grandmaster one step away (the master's Announce stepsRemoved 0)."""

    def _status(self) -> bytes:
        self.updates += 1
        return peer.SHM_STATUS.pack(
            1, self.updates, peer.PORT_SLAVE, 1, 1, 1, 0, 1, 1, 0, self.own, self.gm,
            246, 248, 0xFE, 248, 0x4100, 0, 0, 0, 1,
            200, 1.0, 1.0, 0, 0.0, -3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0)


# ---- the test -------------------------------------------------------------------------

def u16(b: bytes, at: int) -> int:
    return struct.unpack_from(">H", b, at)[0]


def u32(b: bytes, at: int) -> int:
    return struct.unpack_from(">I", b, at)[0]


def u64(b: bytes, at: int) -> int:
    return struct.unpack_from(">Q", b, at)[0]


def enumerate_model(g: peer.Grader, c: Controller, eid: int, dut_mac: bytes) -> dict:
    """READ_DESCRIPTOR of ENTITY, CONFIGURATION 0 and every descriptor it lists; the descriptors by (type, index)."""
    out = {}
    r = c.read(D_ENTITY, 0)
    if not g.check("READ_DESCRIPTOR ENTITY: SUCCESS", r is not None and r["status"] == SUCCESS,
                   str(r and r["status"])):
        return out
    e = r["payload"][4:]                         # past configuration_index and reserved
    out[(D_ENTITY, 0)] = e
    g.check("ENTITY: entity_id is this board's, eth0's MAC expanded (Table 7-2, the same as ADP's)",
            u64(e, 4) == eid, f"{u64(e, 4):016x}")
    g.check("ENTITY: entity_model_id pinned 0x001BC50AC1000100", u64(e, 12) == MODEL_ID, f"{u64(e, 12):016x}")
    g.check("ENTITY: AEM_SUPPORTED in entity_capabilities", u32(e, 20) & 0x8 != 0, f"{u32(e, 20):08x}")
    g.check("ENTITY: 1 talker source, 2 listener sinks", (u16(e, 24), u16(e, 28)) == (1, 2),
            f"{u16(e, 24)}/{u16(e, 28)}")
    g.check("ENTITY: entity_name 'PocketBeagle 2 bridge'", cstr(e[48:112]) == "PocketBeagle 2 bridge",
            repr(cstr(e[48:112])))
    configs, current = u16(e, 308), u16(e, 310)
    g.check("ENTITY: one Configuration, current 0 (IEEE 1722.1-2021 7.2.1)", (configs, current) == (1, 0),
            f"{configs}/{current}")
    r = c.read(D_CONFIGURATION, 0)
    if not g.check("READ_DESCRIPTOR CONFIGURATION 0: SUCCESS", r is not None and r["status"] == SUCCESS):
        return out
    cfg = r["payload"][4:]
    out[(D_CONFIGURATION, 0)] = cfg
    count, offset = u16(cfg, 70), u16(cfg, 72)
    listed = [(u16(cfg, offset + 4 * k), u16(cfg, offset + 4 * k + 2)) for k in range(count)]
    read = failed = 0
    for dtype, n in listed:
        for index in range(n):
            r = c.read(dtype, index)
            body = r["payload"][4:] if r else b""
            if r and r["status"] == SUCCESS and u16(body, 0) == dtype and u16(body, 2) == index:
                out[(dtype, index)] = body
                read += 1
            else:
                failed += 1
                print(f"   READ_DESCRIPTOR {dtype:#06x}/{index}: {r and r['status']}")
    g.check(f"READ_DESCRIPTOR of every descriptor CONFIGURATION 0 lists ({sum(n for _, n in listed)})",
            failed == 0 and read == sum(n for _, n in listed), f"{read} read, {failed} failed; {listed}")
    counts = dict(listed)
    g.check("CONFIGURATION: 2 STREAM_INPUTs, 1 STREAM_OUTPUT, 1 AVB_INTERFACE",
            (counts.get(D_STREAM_INPUT), counts.get(D_STREAM_OUTPUT), counts.get(D_AVB_INTERFACE)) == (2, 1, 1),
            str(counts))
    r = c.read(D_STREAM_INPUT, 2)
    g.check("READ_DESCRIPTOR STREAM_INPUT 2: NO_SUCH_DESCRIPTOR", r is not None and r["status"] == NO_SUCH_DESCRIPTOR)
    r = c.read(D_STREAM_OUTPUT, 1)
    g.check("READ_DESCRIPTOR STREAM_OUTPUT 1 (no CRF output): NO_SUCH_DESCRIPTOR",
            r is not None and r["status"] == NO_SUCH_DESCRIPTOR)
    return out


def check_model(g: peer.Grader, d: dict, eid: int, dut_mac: bytes) -> None:
    """The board's inventory in the descriptors (IEEE 1722.1-2021 Tables 7-8, 7-13; Milan v1.2 6.2, 7.2.2, 7.3.2)."""
    avb = d.get((D_AVB_INTERFACE, 0), b"")
    g.check("AVB_INTERFACE: mac_address is eth0's", avb[70:76] == dut_mac, avb[70:76].hex())
    g.check("AVB_INTERFACE: clock_identity is eth0's EUI-64", len(avb) > 86 and u64(avb, 78) == eid)
    g.check("AVB_INTERFACE: flexptpd's dataset (priority1/2 248, class 248, accuracy 0xFE, variance 0xFFFF, "
            "port 1)", len(avb) > 98 and (avb[86], avb[87], u16(avb, 88), avb[90], avb[91], avb[92], u16(avb, 96))
            == (248, 248, 0xFFFF, 0xFE, 248, 0, 1))
    aaf = None
    for (dtype, index), want in (((D_STREAM_INPUT, 0), "aaf"), ((D_STREAM_INPUT, 1), "crf"),
                                 ((D_STREAM_OUTPUT, 0), "aaf")):
        s = d.get((dtype, index), b"")
        fmt = u64(s, 74) if len(s) >= 82 else 0
        name = f"{'STREAM_INPUT' if dtype == D_STREAM_INPUT else 'STREAM_OUTPUT'} {index}"
        if want == "crf":
            g.check(f"{name}: CRF, current_format {CRF_48K:#018x}", fmt == CRF_48K, f"{fmt:#018x}")
            continue
        # AAF (IEEE 1722-2016 Annex I.2.4: subtype 2, nsr 5 = 48 kHz, format 2 INT32, 32 bits, 8 channels,
        # 6 samples a PDU, Milan v1.2 6.2)
        ok = ((fmt >> 56) & 0x7F == 0x02 and (fmt >> 48) & 0x0F == 0x5 and (fmt >> 40) & 0xFF == 0x02
              and (fmt >> 32) & 0xFF == 32 and (fmt >> 22) & 0x3FF == 8 and (fmt >> 12) & 0x3FF == 6)
        g.check(f"{name}: AAF 48 kHz, 8 channels, 32-bit", ok, f"{fmt:#018x}")
        aaf = aaf or fmt
        if dtype == D_STREAM_OUTPUT:
            g.check("STREAM_OUTPUT 0: the same format as STREAM_INPUT 0", fmt == aaf)
    sources = [d.get((D_CLOCK_SOURCE, k), b"") for k in range(2)]
    g.check("CLOCK_SOURCE 1: the CRF input, located on STREAM_INPUT 1 (Milan v1.2 5.3.3.6)",
            len(sources[1]) >= 86 and (u16(sources[1], 82), u16(sources[1], 84)) == (D_STREAM_INPUT, 1),
            sources[1][70:86].hex() if sources[1] else "absent")


def check_ownership(g: peer.Grader, c: Controller) -> None:
    """ACQUIRE, LOCK and ENTITY_AVAILABLE (Milan v1.2 5.4.2.1 to 5.4.2.3)."""
    r, _ = c.command(ACQUIRE, struct.pack(">IQHH", 0, 0, D_ENTITY, 0))
    g.check("ACQUIRE_ENTITY: NOT_SUPPORTED (Milan v1.2 5.4.2.1)", r is not None and r["status"] == NOT_SUPPORTED,
            str(r and r["status"]))
    r, dt = c.command(LOCK, struct.pack(">IQHH", 0, 0, D_ENTITY, 0))
    g.check("LOCK_ENTITY: SUCCESS, within 240 ms (IEEE 1722.1-2021 9.3.2.6)",
            r is not None and r["status"] == SUCCESS and dt < 0.24, f"{r and r['status']} in {dt * 1000:.0f} ms")
    r, _ = c.command(ENTITY_AVAILABLE, b"\0" * 20)
    g.check("ENTITY_AVAILABLE: locked by this controller", r is not None and r["status"] == SUCCESS
            and u32(r["payload"], 0) & 2 and u64(r["payload"], 12) == peer.CONTROLLER,
            r["payload"].hex() if r else "none")
    r, _ = c.command(SET_NAME, struct.pack(">HHHH", D_ENTITY, 0, 0, 0) + b"other".ljust(64, b"\0"),
                     controller=CONTROLLER_B)
    g.check("SET_NAME from another controller while locked: ENTITY_LOCKED", r is not None
            and r["status"] == ENTITY_LOCKED, str(r and r["status"]))
    r, _ = c.command(LOCK, struct.pack(">IQHH", 1, 0, D_ENTITY, 0))
    g.check("LOCK_ENTITY with UNLOCK: SUCCESS", r is not None and r["status"] == SUCCESS)
    r, _ = c.command(ENTITY_AVAILABLE, b"\0" * 20)
    g.check("ENTITY_AVAILABLE: unlocked", r is not None and r["status"] == SUCCESS and u32(r["payload"], 0) & 2 == 0)


def check_milan_info(g: peer.Grader, c: Controller) -> None:
    """Milan v1.2 5.4.3: the MVU commands."""
    r, _ = c.command(0, b"\0" * 2, mvu=True)
    g.check("MVU GET_MILAN_INFO: SUCCESS, protocol_version 1", r is not None and r["status"] == SUCCESS
            and r["protocol"] == MILAN_PROTOCOL and len(r["payload"]) >= 6 and u32(r["payload"], 2) == 1,
            r["payload"].hex() if r else "none")
    r, _ = c.command(1, b"\0" * 2 + struct.pack(">Q", 0x0123456789ABCDEF), mvu=True)
    g.check("MVU SET_SYSTEM_UNIQUE_ID: SUCCESS", r is not None and r["status"] == SUCCESS)
    r, _ = c.command(2, b"\0" * 2, mvu=True)
    g.check("MVU GET_SYSTEM_UNIQUE_ID: the one set", r is not None and r["status"] == SUCCESS
            and len(r["payload"]) >= 10 and u64(r["payload"], 2) == 0x0123456789ABCDEF)


def check_commands(g: peer.Grader, c: Controller, d: dict, eid: int, dut_mac: bytes) -> None:
    """The Milan v1.2 5.4.2 commands as milan-fpga's AECP implements them."""
    r, _ = c.command(GET_CONFIGURATION, b"\0" * 4)
    g.check("GET_CONFIGURATION: 0", r is not None and r["status"] == SUCCESS and u16(r["payload"], 2) == 0)
    r, _ = c.command(SET_CONFIGURATION, b"\0" * 4)
    g.check("SET_CONFIGURATION 0: SUCCESS", r is not None and r["status"] == SUCCESS, str(r and r["status"]))
    si0 = u64(d.get((D_STREAM_INPUT, 0), b"\0" * 82), 74)
    for dtype, index, want in ((D_STREAM_INPUT, 0, si0), (D_STREAM_INPUT, 1, CRF_48K), (D_STREAM_OUTPUT, 0, si0)):
        r, _ = c.command(GET_STREAM_FORMAT, struct.pack(">HHQ", dtype, index, 0))
        g.check(f"GET_STREAM_FORMAT {dtype:#x}/{index}: the current format", r is not None
                and r["status"] == SUCCESS and u64(r["payload"], 4) == want)
    r, _ = c.command(SET_STREAM_FORMAT, struct.pack(">HHQ", D_STREAM_INPUT, 0, si0))
    g.check("SET_STREAM_FORMAT STREAM_INPUT 0 to its format: SUCCESS", r is not None and r["status"] == SUCCESS,
            str(r and r["status"]))
    two = (si0 & ~(0x3FF << 22)) | (2 << 22)
    r, _ = c.command(SET_STREAM_FORMAT, struct.pack(">HHQ", D_STREAM_INPUT, 0, two))
    g.check("SET_STREAM_FORMAT STREAM_INPUT 0 to 2 channels: refused, the media plane runs one format",
            r is not None and r["status"] != SUCCESS, str(r and r["status"]))
    r, _ = c.command(GET_STREAM_INFO, struct.pack(">HH", D_STREAM_OUTPUT, 0))
    ok = r is not None and r["status"] == SUCCESS
    if g.check("GET_STREAM_INFO STREAM_OUTPUT 0: SUCCESS", ok, str(r and r["status"])):
        p = r["payload"]
        g.check("GET_STREAM_INFO STREAM_OUTPUT 0: stream_id eth0's MAC then 0, VLAN 2, a MAAP address",
                u64(p, 16) == int.from_bytes(dut_mac, "big") << 16 and u16(p, 44) == 2
                and p[28] == 0x91 and p[29] == 0xE0, p[4:48].hex())
    r, _ = c.command(SET_NAME, struct.pack(">HHHH", D_ENTITY, 0, 0, 0) + b"bridge under test".ljust(64, b"\0"))
    g.check("SET_NAME ENTITY: SUCCESS", r is not None and r["status"] == SUCCESS, str(r and r["status"]))
    r, _ = c.command(GET_NAME, struct.pack(">HHHH", D_ENTITY, 0, 0, 0))
    g.check("GET_NAME ENTITY: the name set", r is not None and r["status"] == SUCCESS
            and cstr(r["payload"][8:72]) == "bridge under test")
    c.command(SET_NAME, struct.pack(">HHHH", D_ENTITY, 0, 0, 0) + b"PocketBeagle 2 bridge".ljust(64, b"\0"))
    r, _ = c.command(GET_SAMPLING_RATE, struct.pack(">HHI", D_AUDIO_UNIT, 0, 0))
    g.check("GET_SAMPLING_RATE AUDIO_UNIT 0: 48000", r is not None and r["status"] == SUCCESS
            and u32(r["payload"], 4) & 0x1FFFFFFF == 48000)
    r, _ = c.command(SET_SAMPLING_RATE, struct.pack(">HHI", D_AUDIO_UNIT, 0, 48000))
    g.check("SET_SAMPLING_RATE AUDIO_UNIT 0 48000: SUCCESS", r is not None and r["status"] == SUCCESS)
    r, _ = c.command(GET_CLOCK_SOURCE, struct.pack(">HHHH", D_CLOCK_DOMAIN, 0, 0, 0))
    g.check("GET_CLOCK_SOURCE CLOCK_DOMAIN 0: INTERNAL (0)", r is not None and r["status"] == SUCCESS
            and u16(r["payload"], 4) == 0)
    # AECP takes the CRF source as its model offers it; milan-mediad does not follow it (AECP-STATUS.md)
    r, _ = c.command(SET_CLOCK_SOURCE, struct.pack(">HHHH", D_CLOCK_DOMAIN, 0, 1, 0))
    g.check("SET_CLOCK_SOURCE CLOCK_DOMAIN 0 to CRF (1): SUCCESS, the model's", r is not None
            and r["status"] == SUCCESS, str(r and r["status"]))
    r, _ = c.command(GET_CLOCK_SOURCE, struct.pack(">HHHH", D_CLOCK_DOMAIN, 0, 0, 0))
    g.check("GET_CLOCK_SOURCE CLOCK_DOMAIN 0: CRF (1)", r is not None and r["status"] == SUCCESS
            and u16(r["payload"], 4) == 1)
    r, _ = c.command(SET_CLOCK_SOURCE, struct.pack(">HHHH", D_CLOCK_DOMAIN, 0, 0, 0))
    g.check("SET_CLOCK_SOURCE CLOCK_DOMAIN 0 back to INTERNAL: SUCCESS", r is not None and r["status"] == SUCCESS)
    r, _ = c.command(GET_CONTROL, struct.pack(">HH", D_CONTROL, 0))
    g.check("GET_CONTROL IDENTIFY: SUCCESS, off", r is not None and r["status"] == SUCCESS and r["payload"][4] == 0,
            r["payload"].hex() if r else "none")
    r, _ = c.command(SET_CONTROL, struct.pack(">HHB", D_CONTROL, 0, 0xFF))
    g.check("SET_CONTROL IDENTIFY on: SUCCESS", r is not None and r["status"] == SUCCESS, str(r and r["status"]))
    c.command(SET_CONTROL, struct.pack(">HHB", D_CONTROL, 0, 0))
    for cmd, name in ((START_STREAMING, "START_STREAMING"), (STOP_STREAMING, "STOP_STREAMING")):
        r, _ = c.command(cmd, struct.pack(">HH", D_STREAM_INPUT, 0))
        g.check(f"{name} STREAM_INPUT 0 unbound: SUCCESS (Milan v1.2 5.4.2.19/20)", r is not None
                and r["status"] == SUCCESS, str(r and r["status"]))
    r, _ = c.command(GET_AVB_INFO, struct.pack(">HH", D_AVB_INTERFACE, 0))
    if g.check("GET_AVB_INFO: SUCCESS", r is not None and r["status"] == SUCCESS, str(r and r["status"])):
        p = r["payload"]
        g.check("GET_AVB_INFO: flexptpd's grandmaster, 200 ns link delay, AS_CAPABLE|GPTP_ENABLED, class A PCP 3 "
                "VLAN 2", (u64(p, 4), u32(p, 12), p[17], u16(p, 18), p[21], u16(p, 22)) == (GM, 200, 3, 1, 3, 2),
                p.hex())
    r, _ = c.command(GET_AS_PATH, struct.pack(">HH", 0, 0))
    g.check("GET_AS_PATH: the grandmaster, then this board", r is not None and r["status"] == SUCCESS
            and u16(r["payload"], 2) == 2 and u64(r["payload"], 4) == GM and u64(r["payload"], 12) == eid,
            r["payload"].hex() if r else "none")
    check_counters(g, c)
    r, _ = c.command(GET_AUDIO_MAP, struct.pack(">HHHH", D_STREAM_PORT_INPUT, 0, 0, 0))
    if g.check("GET_AUDIO_MAP STREAM_PORT_INPUT 0: SUCCESS", r is not None and r["status"] == SUCCESS,
               str(r and r["status"])):
        p = r["payload"]
        n = u16(p, 8)
        rows = [struct.unpack_from(">HHHH", p, 12 + 8 * k) for k in range(n)]
        g.check("GET_AUDIO_MAP STREAM_PORT_INPUT 0: the identity of stream channels 0-7", rows ==
                [(0, ch, ch, 0) for ch in range(8)], str(rows))
    # an edit the core validates reaches the face's EDIT window, which the board refuses at its begin
    r, _ = c.command(REMOVE_AUDIO_MAPPINGS, struct.pack(">HHHH", D_STREAM_PORT_INPUT, 0, 1, 0)
                     + struct.pack(">HHHH", 0, 7, 7, 0))
    g.check("REMOVE_AUDIO_MAPPINGS STREAM_PORT_INPUT 0 (an existing mapping): NOT_SUPPORTED (no channel router)",
            r is not None and r["status"] == NOT_SUPPORTED, str(r and r["status"]))
    r, _ = c.command(ADD_AUDIO_MAPPINGS, struct.pack(">HHHH", D_STREAM_PORT_INPUT, 0, 1, 0)
                     + struct.pack(">HHHH", 0, 0, 1, 0))
    g.check("ADD_AUDIO_MAPPINGS STREAM_PORT_INPUT 0 onto a mapped cluster channel: BAD_ARGUMENTS (IEEE 1722.1-2021 "
            "7.4.45)", r is not None and r["status"] == BAD_ARGUMENTS, str(r and r["status"]))
    r, _ = c.command(GET_AUDIO_MAP, struct.pack(">HHHH", D_STREAM_PORT_INPUT, 0, 0, 0))
    g.check("GET_AUDIO_MAP STREAM_PORT_INPUT 0 after the refused edits: still the identity", r is not None
            and r["status"] == SUCCESS and u16(r["payload"], 8) == 8)
    r, _ = c.command(GET_AUDIO_MAP, struct.pack(">HHHH", D_STREAM_PORT_OUTPUT, 0, 0, 0))
    g.check("GET_AUDIO_MAP STREAM_PORT_OUTPUT 0 (it has an AUDIO_MAP): NOT_SUPPORTED (Milan v1.2 5.4.2.26)",
            r is not None and r["status"] == NOT_SUPPORTED, str(r and r["status"]))
    member = struct.pack(">HHBBH", 4, 0, 0, 0, GET_CONFIGURATION) + b"\0" * 4
    r, _ = c.command(GET_DYNAMIC_INFO, member)
    g.check("GET_DYNAMIC_INFO of GET_CONFIGURATION: SUCCESS", r is not None and r["status"] == SUCCESS,
            str(r and r["status"]))
    r, _ = c.command(REBOOT, struct.pack(">HH", D_ENTITY, 0))
    g.check("REBOOT: NOT_IMPLEMENTED (no reset port on this platform)", r is not None
            and r["status"] == NOT_IMPLEMENTED, str(r and r["status"]))


def check_counters(g: peer.Grader, c: Controller) -> None:
    """GET_COUNTERS (Milan v1.2 5.4.2.25, Tables 5.13 to 5.17)."""
    want = {(D_AVB_INTERFACE, 0): 0x23, (D_CLOCK_DOMAIN, 0): 0x3, (D_STREAM_INPUT, 0): 0xF1F,
            (D_STREAM_OUTPUT, 0): 0x13, (D_STREAM_INPUT, 1): 0x0}
    got = {}
    for (dtype, index), valid in want.items():
        r, _ = c.command(GET_COUNTERS, struct.pack(">HH", dtype, index))
        ok = r is not None and r["status"] == SUCCESS
        got[(dtype, index)] = r["payload"] if ok else b""
        g.check(f"GET_COUNTERS {dtype:#x}/{index}: SUCCESS, counters_valid {valid:#x}",
                ok and u32(r["payload"], 4) == valid, f"{r and r['status']} {r and r['payload'][4:8].hex()}")
    si = got.get((D_STREAM_INPUT, 0), b"")
    if len(si) >= 136:
        q = [u32(si, 8 + 4 * k) for k in range(12)]
        g.check("GET_COUNTERS STREAM_INPUT 0: milan-mediad's counters, quadlet by quadlet",
                (q[0], q[1], q[2], q[3], q[4], q[8], q[9], q[10], q[11]) == (1, 6, 8, 2, 7, 5, 3, 4, 8000), str(q))
    so = got.get((D_STREAM_OUTPUT, 0), b"")
    if len(so) >= 136:
        q = [u32(so, 8 + 4 * k) for k in range(5)]
        g.check("GET_COUNTERS STREAM_OUTPUT 0: STREAM_START, STREAM_STOP, FRAMES_TX", (q[0], q[1], q[4]) ==
                (1, 9, 16000), str(q))
    avb = got.get((D_AVB_INTERFACE, 0), b"")
    if len(avb) >= 136:
        g.check("GET_COUNTERS AVB_INTERFACE: LINK_UP 1, GPTP_GM_CHANGED 1", (u32(avb, 8), u32(avb, 28)) == (1, 1),
                f"{u32(avb, 8)} {u32(avb, 28)}")


def check_notifications(g: peer.Grader, c: Controller) -> None:
    """REGISTER_UNSOLICITED_NOTIFICATION, then a change by another controller reaches this one (Milan v1.2
    5.4.2.21, 5.4.5)."""
    r, _ = c.command(REGISTER_UNSOL, b"\0" * 4)
    g.check("REGISTER_UNSOLICITED_NOTIFICATION: SUCCESS", r is not None and r["status"] == SUCCESS)
    t = time.monotonic()
    c.command(SET_NAME, struct.pack(">HHHH", D_ENTITY, 0, 0, 0) + b"renamed".ljust(64, b"\0"),
              controller=CONTROLLER_B)
    n = c.wire.wait(lambda f: is_aecp(f) and parse_aecp(f[14:]).get("unsolicited")
                    and parse_aecp(f[14:])["controller"] == peer.CONTROLLER
                    and parse_aecp(f[14:])["cmd"] == SET_NAME, 1.0, t)
    g.check("an unsolicited SET_NAME notification to the registered controller", n is not None,
            f"after {(n[0] - t) * 1000:.0f} ms" if n else "none in 1 s")
    c.command(SET_NAME, struct.pack(">HHHH", D_ENTITY, 0, 0, 0) + b"PocketBeagle 2 bridge".ljust(64, b"\0"))
    r, dt = c.command(DEREGISTER_UNSOL, b"")
    g.check("DEREGISTER_UNSOLICITED_NOTIFICATION: SUCCESS", r is not None and r["status"] == SUCCESS,
            f"{r and r['status']} after {dt * 1000:.0f} ms")


def bind(g: peer.Grader, c: Controller, wire: peer.Wire, eid: int, a, luid: int, tuid: int, stream: int) -> None:
    """ACMP BIND_RX (CONNECT_RX_COMMAND, Milan v1.2 5.5.2) on STREAM_INPUT luid, its probe of the talker, the
    talker's answer, and the settled sink in the datapath block and in GET_STREAM_INFO."""
    t = time.monotonic()
    seq = 0x6000 + luid
    wire.send(peer.ADP_ACMP_MC, peer.acmpdu(peer.BIND_RX_CMD, controller=peer.CONTROLLER, talker=peer.TALKER_PEER,
                                            listener=eid, tuid=tuid, luid=luid, seq=seq))
    rsp = wire.wait(lambda f: peer.is_sub(peer.SUB_ACMP, peer.BIND_RX_RSP)(f)
                    and peer.parse_acmp(f[14:])["seq"] == seq, 1.0, t)
    g.check(f"ACMP BIND_RX STREAM_INPUT {luid}: SUCCESS", rsp is not None
            and peer.parse_acmp(rsp[1][14:])["status"] == 0, str(rsp and peer.parse_acmp(rsp[1][14:])["status"]))
    probe = wire.wait(lambda f: peer.is_sub(peer.SUB_ACMP, peer.PROBE_TX_CMD)(f)
                      and peer.parse_acmp(f[14:])["luid"] == luid, 1.0, t)
    if not g.check(f"ACMP STREAM_INPUT {luid}: PROBE_TX_COMMAND to talker unique id {tuid}", probe is not None
                   and peer.parse_acmp(probe[1][14:])["tuid"] == tuid):
        return
    q = peer.parse_acmp(probe[1][14:])
    wire.send(peer.ADP_ACMP_MC, peer.acmpdu(peer.PROBE_TX_RSP, stream_id=stream, controller=q["controller"],
                                            talker=peer.TALKER_PEER, listener=eid, tuid=q["tuid"], luid=q["luid"],
                                            dmac=peer.PEER_STREAM_DMAC + luid, seq=q["seq"], vlan=2))
    sink = ""
    for _ in range(50):
        sink = peer.milan_dp(a.dp, a.shm).get(f"sink{luid}", "")
        if f"stream_id:{stream:016x}" in sink and "listening:1" in sink:
            break
        time.sleep(0.02)
    g.check(f"datapath: sink {luid} listening to stream {stream:016x}", f"stream_id:{stream:016x}" in sink
            and "listening:1" in sink, sink)
    r, _ = c.command(GET_STREAM_INFO, struct.pack(">HH", D_STREAM_INPUT, luid))
    ok = r is not None and r["status"] == SUCCESS
    g.check(f"GET_STREAM_INFO STREAM_INPUT {luid}: bound to the stream (CONNECTED, STREAM_ID_VALID)", ok
            and u32(r["payload"], 4) & 0x44000000 == 0x44000000 and u64(r["payload"], 16) == stream,
            r["payload"][4:24].hex() if r else "none")


def unbind(g: peer.Grader, wire: peer.Wire, eid: int, luid: int, tuid: int) -> None:
    t = time.monotonic()
    seq = 0x6100 + luid
    wire.send(peer.ADP_ACMP_MC, peer.acmpdu(peer.UNBIND_RX_CMD, controller=peer.CONTROLLER, talker=peer.TALKER_PEER,
                                            listener=eid, tuid=tuid, luid=luid, seq=seq))
    rsp = wire.wait(lambda f: peer.is_sub(peer.SUB_ACMP, peer.UNBIND_RX_RSP)(f)
                    and peer.parse_acmp(f[14:])["seq"] == seq, 1.0, t)
    g.check(f"ACMP UNBIND_RX STREAM_INPUT {luid}: SUCCESS", rsp is not None
            and peer.parse_acmp(rsp[1][14:])["status"] == 0)


def main() -> int:
    ap = argparse.ArgumentParser()
    for k in ("--dut", "--peer", "--ctrld", "--dp", "--entity", "--tmp"):
        ap.add_argument(k, required=True)
    a = ap.parse_args()
    g = peer.Grader()
    probe = peer.socket.socket(peer.socket.AF_PACKET, peer.socket.SOCK_RAW, 0)
    probe.bind((a.dut, 0))
    dut_mac = probe.getsockname()[4]
    probe.close()
    eid = peer.mac_int(dut_mac[:3] + b"\xff\xfe" + dut_mac[3:])
    a.shm = f"/milan-dp-aecp-{os.getpid()}"
    media_name = f"/milan-media-aecp-{os.getpid()}"
    gptp_name = f"flexptpd-aecp-{os.getpid()}"
    ptp = Flexptpd(gptp_name, eid)
    media = FakeMedia(media_name)
    wire = peer.Wire(a.peer, dut_mac)
    c = Controller(wire, dut_mac, eid, (peer.CONTROLLER, CONTROLLER_B))
    log = open(os.path.join(a.tmp, "ctrld-aecp.log"), "w")
    c.ctrld_pid = 0
    # the earlier image's journal at the path: a KLJ2 file (two 64 KiB slots, its magic in slot A's header)
    journal = os.path.join(a.tmp, "journal-aecp.bin")
    legacy = bytearray(b"\xff" * 0x20000)
    legacy[0:4] = struct.pack("<I", 0x324A4C4B)
    open(journal, "wb").write(legacy)
    proc = subprocess.Popen([a.ctrld, "-i", a.dut, "-e", a.entity, "-g", gptp_name, "-d", a.shm, "-m", media_name,
                             "-N", os.path.join(a.tmp, "journal-aecp.bin"), "-n", "-v"], stdout=log, stderr=log)
    c.ctrld_pid = proc.pid
    try:
        r = wire.wait(lambda f: peer.is_sub(peer.SUB_ADP, peer.ADP_AVAILABLE)(f)
                      and peer.parse_adp(f[14:])["gm"] == GM, 7.0)
        aside = journal + ".klj2"
        g.check("journal: the KLJ2 file moved aside, unchanged, and a KLJ3 journal of 640 KiB in its place",
                os.path.exists(aside) and open(aside, "rb").read() == bytes(legacy)
                and os.path.getsize(journal) == 0xA0000, f"{os.path.getsize(journal):#x}")
        log.flush()
        boot = [line for line in open(os.path.join(a.tmp, "ctrld-aecp.log")) if "saved state at boot" in line]
        g.check("journal: the store read the KLJ2 header as legacy and booted blank (nvm_store.c, nvm_legacy_check)",
                bool(boot) and "legacy 0x1" in boot[0] and "slot none" in boot[0], boot[0].strip() if boot else "")
        if g.check("ADP: ENTITY_AVAILABLE (AECP open before ADP, Milan v1.2 5.6.1)", r is not None):
            p = peer.parse_adp(r[1][14:])
            g.check("ADP: entity_model_id 0x001BC50AC1000100, 1 source, 2 sinks, AEM_SUPPORTED",
                    (p["entity_model_id"], p["talker_stream_sources"], p["listener_stream_sinks"]) == (MODEL_ID, 1, 2)
                    and p["entity_capabilities"] & 0x8 != 0)
        wire.wait(peer.is_sub(peer.SUB_MAAP, peer.MAAP_ANNOUNCE), 6.0)   # the talker's address, for GET_STREAM_INFO
        time.sleep(0.3)                                                    # the media block's first poll
        d = enumerate_model(g, c, eid, dut_mac)
        check_model(g, d, eid, dut_mac)
        check_milan_info(g, c)
        check_ownership(g, c)
        check_commands(g, c, d, eid, dut_mac)
        check_notifications(g, c)
        bind(g, c, wire, eid, a, 0, 0, TALKER_STREAM_AAF)
        bind(g, c, wire, eid, a, 1, 1, TALKER_STREAM_CRF)
        r, _ = c.command(GET_COUNTERS, struct.pack(">HH", D_STREAM_INPUT, 1))
        g.check("GET_COUNTERS STREAM_INPUT 1 (CRF) while bound: SUCCESS", r is not None and r["status"] == SUCCESS)
        unbind(g, wire, eid, 0, 0)
        unbind(g, wire, eid, 1, 1)
        t = time.monotonic()
        proc.send_signal(signal.SIGTERM)
        r = wire.wait(peer.is_sub(peer.SUB_ADP, peer.ADP_DEPARTING), 1.0, t)
        g.check("ADP: ENTITY_DEPARTING on SIGTERM", r is not None)
        rc = proc.wait(timeout=5)
        g.check("milan-ctrld exits 0", rc == 0, f"rc {rc}")
    finally:
        if proc.poll() is None:
            proc.kill()
        c.stop = True
        wire.stop = True
        ptp.close()
        media.close()
        try:
            os.unlink("/dev/shm" + a.shm)
        except OSError:
            pass
        log.close()
    print(f"aecp: {'PASS' if g.failed == 0 else 'FAIL'} ({g.failed} failed)")
    if g.failed:
        print("--- milan-ctrld log ---")
        print(open(os.path.join(a.tmp, "ctrld-aecp.log")).read())
    return 1 if g.failed else 0


if __name__ == "__main__":
    sys.exit(main())
