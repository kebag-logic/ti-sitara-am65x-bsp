#!/usr/bin/env python3
"""Portable foreground review driver. Usage: run-focused.py REPO FIRMWARE PACKET."""
import concurrent.futures
import os
from pathlib import Path
import subprocess
import sys

repo, fw, packet = map(lambda p: Path(p).resolve(), sys.argv[1:])
work = repo / 'milan-linux'
scratch = packet / 'scratch'
env = dict(os.environ, TMPDIR=str(scratch / 'tmp'), PYTHONDONTWRITEBYTECODE='1')
(scratch / 'tmp').mkdir(parents=True, exist_ok=True)

def run(name, argv, timeout=540):
    with (packet / (name + '.log')).open('w') as log:
        log.write('cwd: milan-linux\ncommand: ' + ' '.join(map(str, argv)) + '\n')
        log.flush()
        try:
            result = subprocess.run(list(map(str, argv)), cwd=work, env=env,
                                    stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
            rc = result.returncode
        except subprocess.TimeoutExpired:
            log.write('TIMEOUT\n')
            rc = 124
    (packet / (name + '.rc')).write_text(str(rc) + '\n')
    print(name, rc, flush=True)
    return rc

# The driver stays in the foreground and joins all independent work.
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
    native = pool.submit(run, 'native-build', ['make', '-j16', 'MILAN_FPGA=' + str(fw),
        'O=' + str(scratch / 'native'), 'all', str(scratch / 'native/test_media'), str(scratch / 'native/test_gptp')])
    config = pool.submit(run, 'generated-config', ['make', '-j16', 'MILAN_FPGA=' + str(fw), 'check-config'])
    built, generated = native.result(), config.result()
if built:
    sys.exit(1)
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    jobs = [pool.submit(run, 'aecp-veth', ['tests/run-netns-aecp.sh', scratch / 'native/milan-ctrld',
        scratch / 'native/milan-dp', 'config/entity.conf']),
        pool.submit(run, 'control-veth', ['tests/run-netns.sh', scratch / 'native/milan-ctrld',
        scratch / 'native/milan-dp', 'config/entity.conf']),
        pool.submit(run, 'media-unit', [scratch / 'native/test_media']),
        pool.submit(run, 'gptp-unit', [scratch / 'native/test_gptp'])]
    results = [j.result() for j in jobs]
sys.exit(int(bool(generated or any(results))))
