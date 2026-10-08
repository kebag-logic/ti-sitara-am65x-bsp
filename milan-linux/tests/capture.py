#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0
"""capture.py - record the frames an interface receives into a pcap.

Nanosecond timestamps in TAI (CLOCK_REALTIME plus the kernel's TAI offset), the
time base the bridge stamps AVTP with when it has no PTP clock, so the
validation kit's latency-peer.py reads them as it reads an I210's. A VLAN tag the
device took off the frame (PACKET_AUXDATA) is put back, as tcpdump does.

usage: capture.py IFACE OUT.pcap SECONDS [ETHERTYPE]
"""

import ctypes
import ctypes.util
import socket
import struct
import sys
import time

SOL_PACKET = 263
PACKET_AUXDATA = 8
TP_STATUS_VLAN_VALID = 1 << 4
SO_TIMESTAMPNS = 35


class Timex(ctypes.Structure):
    _fields_ = [("modes", ctypes.c_uint), ("offset", ctypes.c_long), ("freq", ctypes.c_long),
                ("maxerror", ctypes.c_long), ("esterror", ctypes.c_long), ("status", ctypes.c_int),
                ("constant", ctypes.c_long), ("precision", ctypes.c_long), ("tolerance", ctypes.c_long),
                ("time_sec", ctypes.c_long), ("time_usec", ctypes.c_long), ("tick", ctypes.c_long),
                ("ppsfreq", ctypes.c_long), ("jitter", ctypes.c_long), ("shift", ctypes.c_int),
                ("stabil", ctypes.c_long), ("jitcnt", ctypes.c_long), ("calcnt", ctypes.c_long),
                ("errcnt", ctypes.c_long), ("stbcnt", ctypes.c_long), ("tai", ctypes.c_int),
                ("pad", ctypes.c_int * 11)]


def tai_offset() -> int:
    libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
    tx = Timex()
    libc.adjtimex(ctypes.byref(tx))
    return tx.tai


def main() -> int:
    ifname, out, seconds = sys.argv[1], sys.argv[2], float(sys.argv[3])
    ethertype = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x22F0
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
    s.bind((ifname, 3))
    s.setsockopt(SOL_PACKET, PACKET_AUXDATA, 1)
    # 8000 frames/s into Python: as much room for its stalls as an unprivileged
    # socket gets (net.core.rmem_max caps it, about 50 ms of frames by default)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16 << 20)
    s.setsockopt(socket.SOL_SOCKET, SO_TIMESTAMPNS, 1)
    s.settimeout(0.2)
    tai = tai_offset() * 1_000_000_000
    f = open(out, "wb")
    # classic pcap, nanosecond resolution, Ethernet
    f.write(struct.pack("<IHHiIII", 0xA1B23C4D, 2, 4, 0, 0, 65535, 1))
    # Frames are handled the same way through a 0.3 s warm-up and only kept
    # after it, so the interpreter's first-pass stalls fall outside the record.
    start = time.monotonic() + 0.3
    end = start + seconds
    n = 0
    while time.monotonic() < end:
        try:
            data, anc, _flags, addr = s.recvmsg(2048, 1024)
        except socket.timeout:
            continue
        if addr[2] == socket.PACKET_OUTGOING:
            continue
        ts = None
        tci = None
        for level, kind, val in anc:
            if level == socket.SOL_SOCKET and kind == SO_TIMESTAMPNS:
                sec, nsec = struct.unpack("qq", val[:16])
                ts = sec * 1_000_000_000 + nsec + tai
            elif level == SOL_PACKET and kind == PACKET_AUXDATA:
                status, _len, _snap, _mac, _net, vtci, vtpid = struct.unpack("IIIHHHH", val[:20])
                if status & TP_STATUS_VLAN_VALID:
                    tci = (vtpid or 0x8100, vtci)
        if tci is not None:
            data = data[:12] + struct.pack(">HH", tci[0], tci[1]) + data[12:]
        inner = struct.unpack(">H", data[16:18])[0] if data[12:14] == b"\x81\x00" else struct.unpack(">H", data[12:14])[0]
        if inner != ethertype or ts is None or time.monotonic() < start:
            continue
        f.write(struct.pack("<IIII", ts // 1_000_000_000, ts % 1_000_000_000, len(data), len(data)))
        f.write(data)
        n += 1
    f.close()
    print(f"capture: {n} frames in {seconds:.0f} s on {ifname} -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
