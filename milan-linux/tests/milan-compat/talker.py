#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0
"""talker.py - a talker for the compatibility run: BIND_RX of the DUT's STREAM_INPUT 0 to an AAF stream and of
STREAM_INPUT 1 to a CRF stream, both sent as tests/counters.py sends them, PROBE_TX answered whenever asked,
until SIGTERM. It sends no ADP, so the controller under test has one entity to judge.

usage: talker.py IFACE DUT_MAC_HEX
"""

import importlib.util
import signal
import sys
import threading
import time
from pathlib import Path

sys.dont_write_bytecode = True
_spec = importlib.util.spec_from_file_location("counters", Path(__file__).resolve().parent.parent / "counters.py")
counters = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(counters)
peer, aecp = counters.peer, counters.aecp

stop = False


def on_term(*_):
    global stop
    stop = True


def main() -> int:
    signal.signal(signal.SIGTERM, on_term)
    ifname, dut_mac = sys.argv[1], bytes.fromhex(sys.argv[2])
    eid = peer.mac_int(dut_mac[:3] + b"\xff\xfe" + dut_mac[3:])
    wire = peer.Wire(ifname, dut_mac)
    streams = {0: aecp.TALKER_STREAM_AAF, 1: aecp.TALKER_STREAM_CRF}
    aaf = counters.Stream(streams[0], peer.PEER_STREAM_DMAC + 0)
    crf = counters.Stream(streams[1], peer.PEER_STREAM_DMAC + 1)
    talker = counters.Talker(ifname, aaf, crf)
    answered = 0

    def answer() -> None:
        # each PROBE_TX_COMMAND, as soon as it is seen (the listener times a probe out)
        nonlocal answered
        while not stop:
            with wire.lock:
                frames = list(wire.frames)
                wire.frames.clear()
            for _, f in frames:
                if not peer.is_sub(peer.SUB_ACMP, peer.PROBE_TX_CMD)(f):
                    continue
                q = peer.parse_acmp(f[14:])
                if q["tuid"] in streams:
                    wire.send(peer.ADP_ACMP_MC, peer.acmpdu(
                        peer.PROBE_TX_RSP, stream_id=streams[q["tuid"]], controller=q["controller"],
                        talker=peer.TALKER_PEER, listener=eid, tuid=q["tuid"], luid=q["luid"],
                        dmac=peer.PEER_STREAM_DMAC + q["tuid"], seq=q["seq"], vlan=2))
                    answered += 1
            time.sleep(0.005)

    responder = threading.Thread(target=answer, daemon=True)
    responder.start()
    for luid in (0, 1):
        wire.send(peer.ADP_ACMP_MC, peer.acmpdu(peer.BIND_RX_CMD, controller=peer.CONTROLLER, talker=peer.TALKER_PEER,
                                                listener=eid, tuid=luid, luid=luid, seq=0x7000 + luid))
        time.sleep(0.5)
    while not stop:
        time.sleep(0.1)
    responder.join(timeout=1)
    talker.close()
    wire.stop = True
    print(f"talker: {answered} PROBE_TX answered, {aaf.sent} AAF and {crf.sent} CRF PDUs sent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
