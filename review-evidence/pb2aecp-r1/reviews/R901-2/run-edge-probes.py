#!/usr/bin/env python3
"""Probe unmodified daemons via disposable copies of the public wire fixture.
Usage: run-edge-probes.py REPO PACKET
"""
from pathlib import Path
import os, shutil, subprocess, sys
repo, packet = map(lambda p: Path(p).resolve(), sys.argv[1:])
work = packet/'scratch/edge-tests'
shutil.copytree(repo/'milan-linux/tests', work, dirs_exist_ok=True)
source = (work/'counters.py').read_text()
start = source.index('        def step(')
end = source.index('    finally:', start)
probe = '''        before = read(c, 1)
        print("EARLY baseline CRF", before, flush=True)
        talker.fault([(aaf, "early", 100000), (crf, "early", 100000)])
        time.sleep(1.5)
        after = read(c, 1)
        dp = peer.milan_dp(a.dp, a.shm)
        print("EARLY sustained CRF", after, flush=True)
        print("EARLY media", {k:v for k,v in dp.items() if k in ("crf_active", "crf_locked", "listener_active", "listener_locked")}, flush=True)
        g.check("CRF early-only stream interrupts and unlocks after receive timeout",
                after[2] == before[2]+1 and after[0] == after[1], str(after))
        talker.fault([(aaf, "early", 0), (crf, "early", 0)])
        time.sleep(0.8)
        # Queue both commands while the control daemon is stopped, then let
        # its normal service loop consume the burst. No source is mutated.
        before_sink = peer.milan_dp(a.dp, a.shm)["sink1"]
        qbefore = read(c, 1)
        ctrld.send_signal(signal.SIGSTOP)
        os.waitpid(ctrld.pid, os.WUNTRACED)
        t = time.monotonic()
        for typ, seq in ((peer.UNBIND_RX_CMD, 0x7001), (peer.BIND_RX_CMD, 0x7002)):
            wire.send(peer.ADP_ACMP_MC, peer.acmpdu(typ, controller=peer.CONTROLLER,
                talker=peer.TALKER_PEER, listener=eid, tuid=1, luid=1, seq=seq))
        time.sleep(0.03)
        ctrld.send_signal(signal.SIGCONT)
        for typ, seq in ((peer.UNBIND_RX_RSP, 0x7001), (peer.BIND_RX_RSP, 0x7002)):
            r = wire.wait(lambda f: peer.is_sub(peer.SUB_ACMP, typ)(f)
                and peer.parse_acmp(f[14:])["seq"] == seq, 1.0, t)
            g.check("burst command SUCCESS " + str(typ), r is not None and peer.parse_acmp(r[1][14:])["status"] == 0)
        time.sleep(0.3)
        after_sink = peer.milan_dp(a.dp, a.shm)["sink1"]
        qafter = read(c, 1)
        print("REBIND before", before_sink, qbefore, flush=True)
        print("REBIND after", after_sink, qafter, flush=True)
        g.check("burst rebind counted", "binds:2" in after_sink)
        g.check("burst rebind zeroed earlier EARLY_TIMESTAMP", qafter[10] == 0, str(qafter))
        # Positive control: separate the same transitions across loop passes.
        aecp.unbind(g, wire, eid, 1, 1)
        time.sleep(0.3)
        aecp.bind(g, c, wire, eid, a, 1, 1, aecp.TALKER_STREAM_CRF)
        time.sleep(0.7)
        serial = read(c, 1)
        print("SERIAL after", peer.milan_dp(a.dp, a.shm)["sink1"], serial, flush=True)
        g.check("separated rebind zeroes earlier EARLY_TIMESTAMP", serial[10] == 0)
'''
(work/'counters.py').write_text(source[:start]+probe+source[end:])
env = dict(os.environ, TMPDIR=str(packet/'scratch'), PYTHONDONTWRITEBYTECODE='1')
cmd = ['sh', str(work/'run-netns-counters.sh'), str(packet/'scratch/native'), str(repo/'milan-linux/config/entity.conf')]
with (packet/'edge-probes.log').open('w') as log:
    log.write('command: '+' '.join(cmd)+'\n'); log.flush()
    rc = subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=90).returncode
(packet/'edge-probes.rc').write_text(str(rc)+'\n')
print('edge probes rc', rc)
