#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0
"""acmp.py - send one ACMP command (IEEE 1722.1-2021 8.2, Milan v1.2 5.5) as a
controller, or as a listener probing a talker, and print the response.

  acmp.py IFACE probe-tx  TALKER_EID TALKER_UID            PROBE_TX_COMMAND
  acmp.py IFACE bind-rx   LISTENER_EID LISTENER_UID TALKER_EID TALKER_UID
  acmp.py IFACE unbind-rx LISTENER_EID LISTENER_UID
  acmp.py IFACE rx-state  LISTENER_EID LISTENER_UID         GET_RX_STATE_COMMAND
  acmp.py IFACE tx-state  TALKER_EID TALKER_UID             GET_TX_STATE_COMMAND

Entity IDs in hex. Exit 0 on a SUCCESS response, 1 on another status or none
within 1 s. Needs CAP_NET_RAW (root).
"""

from __future__ import annotations

import socket
import struct
import sys
import time

ADP_ACMP_MC = bytes.fromhex("91e0f0010000")
ETH_P_ALL = 3
ETHERTYPE_AVTP = 0x22F0
ETHERTYPE_VLAN = b"\x81\x00"
SUBTYPE_ACMP = 0xFC

SOL_PACKET = 263
PACKET_ADD_MEMBERSHIP = 1
PACKET_MR_MULTICAST = 0

CONTROLLER = 0x0200C0FFEE0000A1
LISTENER = 0x0200C0FFEE0000A2          # the listener a probe claims to be

# command message types; the response is the command + 1
CMDS = {
    "probe-tx": 0,
    "tx-state": 4,
    "bind-rx": 6,
    "unbind-rx": 8,
    "rx-state": 10,
}

STATUS = {
    0: "SUCCESS",
    1: "LISTENER_UNKNOWN_ID",
    2: "TALKER_UNKNOWN_ID",
    3: "TALKER_DEST_MAC_FAIL",
    7: "LISTENER_TALKER_TIMEOUT",
    16: "CONTROLLER_NOT_AUTHORIZED",
    17: "INCOMPATIBLE_REQUEST",
    31: "NOT_SUPPORTED",
}


def build_pdu(mtype: int, talker: int, tuid: int, listener: int, luid: int, seq: int) -> bytes:
    """The Milan ACMPDU (56 bytes from the subtype on)."""
    header = struct.pack(
        ">BBH",
        SUBTYPE_ACMP,
        mtype,                          # sv 0, version 0, message_type
        44,                             # status 0, control_data_length
    )

    ids = struct.pack(
        ">QQQQHH",
        0,                              # stream_id
        CONTROLLER,
        talker,
        listener,
        tuid,
        luid,
    )

    dest_mac = b"\0" * 6

    tail = struct.pack(
        ">HHHHH",
        0,                              # connection_count
        seq,                            # sequence_id
        0,                              # flags
        0,                              # stream_vlan_id
        0,                              # reserved
    )

    return header + ids + dest_mac + tail


def open_socket(ifname: str) -> socket.socket:
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
    s.bind((ifname, ETH_P_ALL))

    # the NIC filters multicast it was not asked for: join the ADP/ACMP group
    mreq = struct.pack(
        "iHH8s",
        socket.if_nametoindex(ifname),
        PACKET_MR_MULTICAST,
        6,
        ADP_ACMP_MC + b"\0\0",
    )
    s.setsockopt(SOL_PACKET, PACKET_ADD_MEMBERSHIP, mreq)

    s.settimeout(0.1)
    return s


def response_to(d: bytes, mtype: int, seq: int) -> bytes | None:
    """The frame, untagged, if it is the response to our command; else None."""
    # a priority tag may still be inline here
    if d[12:14] == ETHERTYPE_VLAN:
        d = d[:12] + d[16:]

    if len(d) < 70:
        return None
    if struct.unpack(">H", d[12:14])[0] != ETHERTYPE_AVTP:
        return None
    if d[14] != SUBTYPE_ACMP:
        return None
    if (d[15] & 0x0F) != mtype + 1:
        return None
    if struct.unpack(">H", d[62:64])[0] != seq:
        return None

    return d


def describe(cmd: str, d: bytes, elapsed_ms: float) -> int:
    """Print the response; its status code."""
    status = d[16] >> 3

    stream, _controller, talker, listener, tuid, luid = struct.unpack(">QQQQHH", d[18:54])
    dest_mac = d[54:60].hex(":")
    count, _seq, flags, vlan = struct.unpack(">HHHH", d[60:68])

    print(
        f"{cmd}: {STATUS.get(status, status)} after {elapsed_ms:.1f} ms"
        f" | stream {stream:016x} dest {dest_mac} vlan {vlan}"
        f" | talker {talker:016x}/{tuid} listener {listener:016x}/{luid}"
        f" | count {count} flags 0x{flags:04x}"
    )
    return status


def main() -> int:
    if len(sys.argv) < 5 or sys.argv[2] not in CMDS:
        print(__doc__)
        return 2

    ifname = sys.argv[1]
    cmd = sys.argv[2]
    args = sys.argv[3:]

    talker = 0
    tuid = 0
    listener = 0
    luid = 0

    if cmd in ("probe-tx", "tx-state"):
        talker = int(args[0], 16)
        tuid = int(args[1])
        listener = LISTENER
    else:
        listener = int(args[0], 16)
        luid = int(args[1])

        if cmd == "bind-rx":
            talker = int(args[2], 16)
            tuid = int(args[3])

    mtype = CMDS[cmd]
    seq = int(time.time() * 1000) & 0xFFFF
    pdu = build_pdu(mtype, talker, tuid, listener, luid, seq)

    s = open_socket(ifname)
    own_mac = s.getsockname()[4]
    frame = ADP_ACMP_MC + own_mac + struct.pack(">H", ETHERTYPE_AVTP) + pdu
    frame += b"\0" * max(0, 60 - len(frame))

    t0 = time.monotonic()
    s.send(frame)

    while time.monotonic() < t0 + 1.0:
        try:
            d = s.recv(2048)
        except socket.timeout:
            continue

        d = response_to(d, mtype, seq)
        if d is None:
            continue

        status = describe(cmd, d, (time.monotonic() - t0) * 1000)
        return 0 if status == 0 else 1

    print(f"{cmd}: no response in 1 s")
    return 1


if __name__ == "__main__":
    sys.exit(main())
