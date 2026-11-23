#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0
"""rxfail.py - a media input whose receive socket cannot be set up reports none of its counters valid.

milan-ctrld runs on the DUT side of the veth pair run-netns-rxfail.sh makes,
and milan-mediad is started again for each case, its talker, AAF listener and
CRF receive path on. rxfail.so (tests/rxfail.c) makes one setup step fail in
one of its receiving threads; a controller on the peer side reads
GET_COUNTERS:

  control             nothing fails: STREAM_INPUT 0 and 1 valid 0xf3f
  each step, each     interface, socket, filter, bind, receive timeout, in
  input               the listener (STREAM_INPUT 0) or the CRF path
                      (STREAM_INPUT 1): that input none valid, the other 0xf3f,
                      milan-mediad still running, the failure logged with its
                      step, and the descriptor it opened closed
  no interface        milan-mediad on an interface that does not exist: both
                      inputs none valid, both failures logged

Usage (as root in the namespace): rxfail.py --dut veth0 --peer veth1 --build DIR --entity FILE --tmp DIR
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import re
import signal
import subprocess
import sys
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

THREADS = (("milan-listener", 0, "listener on"), ("milan-crf-rx", 1, "CRF input on"))
STEPS = (("interface", "interface"), ("socket", "socket"), ("filter", "filter"), ("bind", "bind"),
         ("timeout", "receive timeout"))


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
    shm = f"/milan-dp-rxf-{os.getpid()}"
    media_name = f"/milan-media-rxf-{os.getpid()}"
    gptp_name = f"flexptpd-rxf-{os.getpid()}"
    ptp = aecp.Flexptpd(gptp_name, eid)
    wire = peer.Wire(a.peer, dut_mac)
    c = aecp.Controller(wire, dut_mac, eid, (peer.CONTROLLER,))
    ctrld_log = open(os.path.join(a.tmp, "ctrld-rxf.log"), "w")
    ctrld = subprocess.Popen([os.path.join(a.build, "milan-ctrld"), "-i", a.dut, "-e", a.entity, "-g", gptp_name,
                              "-d", shm, "-m", media_name, "-N", "none", "-n"], stdout=ctrld_log, stderr=ctrld_log)
    shim = os.path.join(a.build, "rxfail.so")
    last = ""

    def run(name: str, ifname: str, env: dict) -> tuple[bool, dict, str]:
        """milan-mediad for one case: alive after 1 s, each input's counters_valid, its log."""
        nonlocal last
        path = os.path.join(a.tmp, f"mediad-rxf-{name}.log")
        with open(path, "w") as log:
            p = subprocess.Popen([os.path.join(a.build, "milan-mediad"), "-i", ifname, "-r", "talker,listener,crf",
                                  "-S", "0", "-P", "0", "-d", shm, "-m", media_name],
                                 stdout=log, stderr=log, env={**os.environ, **env})
            time.sleep(1.0)
            alive = p.poll() is None
            valid = {k: aecp.counters(c, aecp.D_STREAM_INPUT, k)[0] for k in (0, 1)}
            p.send_signal(signal.SIGTERM)
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        last = open(path).read()
        return alive, valid, last

    try:
        r = wire.wait(lambda f: peer.is_sub(peer.SUB_ADP, peer.ADP_AVAILABLE)(f), 7.0)
        g.check("ADP: ENTITY_AVAILABLE", r is not None)
        alive, valid, log = run("control", a.dut, {"LD_PRELOAD": shim, "RXFAIL_THREAD": "milan-listener",
                                                   "RXFAIL_STEP": "none"})
        g.check("control, nothing fails: STREAM_INPUT 0 and 1 valid 0xf3f, nothing logged as failed",
                alive and valid == {0: 0xF3F, 1: 0xF3F} and "not received" not in log and "failed" not in log,
                f"alive {alive} valid {valid}")
        for thread, index, says in THREADS:
            other = 1 - index
            for step, logged in STEPS:
                alive, valid, log = run(f"{thread}-{step}", a.dut, {"LD_PRELOAD": shim, "RXFAIL_THREAD": thread,
                                                                     "RXFAIL_STEP": step})
                name = f"{thread} {step} fails"
                g.check(f"{name}: injected", f"rxfail: {thread} {step} failed" in log)
                g.check(f"{name}: STREAM_INPUT {index} none valid, STREAM_INPUT {other} 0xf3f, milan-mediad "
                        "running", alive and valid.get(index) == 0 and valid.get(other) == 0xF3F,
                        f"alive {alive} valid {valid}")
                g.check(f"{name}: logged with its step", f"{says} {a.dut}: {logged}:" in log
                        and f"STREAM_INPUT {index} is not received, none of its counters valid" in log,
                        next((x for x in log.splitlines() if "not received" in x), "no line"))
                opened = re.search(rf"rxfail: {thread} opened fd (\d+)", log)
                if step in ("interface", "socket"):
                    g.check(f"{name}: no descriptor opened", opened is None)
                else:
                    g.check(f"{name}: the descriptor it opened is closed", opened is not None
                            and f"rxfail: {thread} closed fd {opened.group(1)}" in log)
        alive, valid, log = run("missing", "missing0", {})
        g.check("no such interface: STREAM_INPUT 0 and 1 none valid, milan-mediad running",
                alive and valid == {0: 0, 1: 0}, f"alive {alive} valid {valid}")
        g.check("no such interface: both failures logged",
                "listener on missing0: interface:" in log and "CRF input on missing0: interface:" in log)
    finally:
        if ctrld.poll() is None:
            ctrld.send_signal(signal.SIGTERM)
            try:
                ctrld.wait(timeout=5)
            except subprocess.TimeoutExpired:
                ctrld.kill()
        c.stop = True
        wire.stop = True
        ptp.close()
        for name in (shm, media_name):
            try:
                os.unlink("/dev/shm" + name)
            except OSError:
                pass
        ctrld_log.close()
    print(f"rxfail: {'PASS' if g.failed == 0 else 'FAIL'} ({g.failed} failed)")
    if g.failed:
        print("--- last milan-mediad log ---")
        print(last[-4000:])
    return 1 if g.failed else 0


if __name__ == "__main__":
    sys.exit(main())
