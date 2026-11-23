#!/usr/bin/env python3
"""Check counter validity when the media receive socket cannot initialize.
Usage: run-startup-probe.py REPO PACKET
"""
from pathlib import Path
import os, shutil, subprocess, sys
repo, packet = map(lambda p: Path(p).resolve(), sys.argv[1:])
work = packet/'scratch/startup-tests'
shutil.copytree(repo/'milan-linux/tests', work, dirs_exist_ok=True)
s = (work/'counters.py').read_text()
old = 'os.path.join(a.build, "milan-mediad"), "-i", a.dut'
assert s.count(old) == 1
s = s.replace(old, 'os.path.join(a.build, "milan-mediad"), "-i", "missing0"')
start = s.index('        before = [read(c, 0), read(c, 1)]')
end = s.index('    finally:',start)
s = s[:start]+'''        print("mediad process still running:", mediad.poll() is None, flush=True)
        for index in (0, 1):
            valid, q = aecp.counters(c, aecp.D_STREAM_INPUT, index)
            print("missing interface STREAM_INPUT", index, "valid", hex(valid), "values", q, flush=True)
            g.check("failed receive path has no valid counters " + str(index), valid == 0)
'''+s[end:]
(work/'counters.py').write_text(s)
cmd = ['sh',str(work/'run-netns-counters.sh'),str(packet/'scratch/native'),str(repo/'milan-linux/config/entity.conf')]
with (packet/'startup-probe.log').open('w') as log:
    log.write('command: '+' '.join(cmd)+'\n'); log.flush()
    rc = subprocess.run(cmd, env=dict(os.environ,TMPDIR=str(packet/'scratch'),PYTHONDONTWRITEBYTECODE='1'),stdout=log,stderr=subprocess.STDOUT,timeout=30).returncode
(packet/'startup-probe.rc').write_text(str(rc)+'\n')
print('startup probe rc',rc)
