#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0
"""bind.py - a controller's Milan BIND_RX (IEEE 1722.1-2021 8.2.1, Milan v1.2
5.5.2.4): bind a listener's STREAM_INPUT to a talker's STREAM_OUTPUT, and wait
for the listener's BIND_RX_RESPONSE.

usage: bind.py IFACE LISTENER_EID TALKER_EID [LISTENER_UID] [TALKER_UID]
Exit 0 on a SUCCESS response, 1 otherwise.
"""

import socket
import struct
import sys
import time

ADP_ACMP_MC = bytes.fromhex("91e0f0010000")
CONTROLLER = 0x0200C0FFEE000001


def main() -> int:
    ifname, listener, talker = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
    luid = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    tuid = int(sys.argv[5]) if len(sys.argv) > 5 else 0
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
    s.bind((ifname, 3))
    s.settimeout(0.1)
    src = s.getsockname()[4]
    seq = 0x6161
    pdu = (struct.pack(">BBHQQQQHH", 0xFC, 6, 44, 0, CONTROLLER, talker, listener, tuid, luid)
           + b"\0" * 6 + struct.pack(">HHHHH", 0, seq, 0, 0, 0))
    frame = ADP_ACMP_MC + src + struct.pack(">H", 0x22F0) + pdu
    s.send(frame + b"\0" * max(0, 60 - len(frame)))
    end = time.monotonic() + 1.0
    while time.monotonic() < end:
        try:
            d = s.recv(2048)
        except socket.timeout:
            continue
        if len(d) < 70 or d[12:14] != b"\x22\xf0" or d[14] != 0xFC or (d[15] & 0x0F) != 7:
            continue
        if struct.unpack(">H", d[62:64])[0] != seq:
            continue
        status = d[16] >> 3
        print(f"bind: BIND_RX_RESPONSE status {status}")
        return 0 if status == 0 else 1
    print("bind: no BIND_RX_RESPONSE in 1 s")
    return 1


if __name__ == "__main__":
    sys.exit(main())
