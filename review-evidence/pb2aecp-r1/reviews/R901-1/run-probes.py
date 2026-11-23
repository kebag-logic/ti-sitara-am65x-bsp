#!/usr/bin/env python3
"""Run disposable identity mutation and stale-status probes; never edit REPO.
Usage: run-probes.py REPO FIRMWARE PACKET
"""
import concurrent.futures
import os
from pathlib import Path
import shutil
import subprocess
import sys

repo, fw, packet = map(lambda x: Path(x).resolve(), sys.argv[1:])
scratch = packet / 'scratch'
work = scratch / 'probe-copy'
shutil.copytree(repo / 'milan-linux', work, dirs_exist_ok=True,
                ignore=shutil.ignore_patterns('build', '.milan-fpga', '__pycache__'))
env = dict(os.environ, TMPDIR=str(scratch / 'tmp'), PYTHONDONTWRITEBYTECODE='1')
def run(name, args, cwd=work):
    with (packet / (name + '.log')).open('w') as log:
        log.write('command: ' + ' '.join(map(str, args)) + '\n')
        log.flush()
        try:
            rc = subprocess.run(list(map(str,args)), cwd=cwd, env=env, stdout=log,
                                stderr=subprocess.STDOUT, timeout=540).returncode
        except subprocess.TimeoutExpired:
            log.write('TIMEOUT\n')
            rc = 124
    (packet / (name + '.rc')).write_text(str(rc) + '\n')
    print(name, rc, flush=True)
    return rc

# Mutation: omit all three runtime identity substitutions, keeping CRC loading.
source = work / 'src/milan_ctrld.c'
original = source.read_text()
mutated = '\n'.join(l for l in original.split('\n') if not l.startswith('\twire_put_be(board_image +'))
assert original.count('\twire_put_be(board_image +') == 3
# Preserve the now-unused offset calculations without enabling build warnings.
mutated = mutated.replace('\tmemcpy(board_image,', '\t(void)ent; (void)mac;\n\tmemcpy(board_image,')
source.write_text(mutated)
out = scratch / 'mutant'
cross = scratch / 'cross'
status = run('identity-mutant-build', ['make','-j16','MILAN_FPGA='+str(fw),
    'O='+str(out), str(out/'milan-ctrld'), str(out/'milan-dp')])
if status:
    sys.exit(1)

wrapper = work / 'tests/run-netns-aecp.sh'
changed = work / 'tests/run-netns-aecp-distinct.sh'
changed.write_text(wrapper.read_text().replace('02:00:00:00:00:01', '02:1a:2b:3c:4d:5e'))
changed.chmod(0o755)

# A fault equivalent to a stopped gPTP publisher: stop its updates, keep its
# last consistent shared-memory block. Check for fabricated lock transitions.
test = (work/'tests/aecp.py').read_text()
injection = '''        ptp.stop = True
        time.sleep(0.2)
        before, _ = c.command(GET_COUNTERS, struct.pack(">HH", D_CLOCK_DOMAIN, 0))
        assert before is not None and before["status"] == SUCCESS
        base_locks = u32(before["payload"], 8)
        base_unlocks = u32(before["payload"], 12)
        print(f"stale probe before: locks={base_locks} unlocks={base_unlocks}", flush=True)
        time.sleep(6.5)
        after, _ = c.command(GET_COUNTERS, struct.pack(">HH", D_CLOCK_DOMAIN, 0))
        assert after is not None and after["status"] == SUCCESS
        locks = u32(after["payload"], 8)
        unlocks = u32(after["payload"], 12)
        print(f"stale probe after: locks={locks} unlocks={unlocks}", flush=True)
        g.check("no new LOCKED transitions without a live gPTP update", locks == base_locks)
'''
assert test.count('        t = time.monotonic()\n        proc.send_signal') == 1
test = test.replace('        t = time.monotonic()\n        proc.send_signal', injection + '        t = time.monotonic()\n        proc.send_signal')
(work/'tests/aecp-stale.py').write_text(test)
stale = work/'tests/run-netns-aecp-stale.sh'
stale.write_text(wrapper.read_text().replace('"$HERE/aecp.py"', '"$HERE/aecp-stale.py"'))
stale.chmod(0o755)

native = scratch/'native'
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    tasks = [pool.submit(run,'identity-mutant-original-test',[wrapper,out/'milan-ctrld',out/'milan-dp','config/entity.conf']),
        pool.submit(run,'identity-mutant-distinct-test',[changed,out/'milan-ctrld',out/'milan-dp','config/entity.conf']),
        pool.submit(run,'identity-pristine-distinct-test',[changed,native/'milan-ctrld',native/'milan-dp','config/entity.conf']),
        pool.submit(run,'gptp-stale-test',[stale,native/'milan-ctrld',native/'milan-dp','config/entity.conf'])]
    results = [task.result() for task in tasks]
print('probe return codes:',results, '(expected baseline 0, mutant/distinct 1, pristine/distinct 0; stale is diagnostic)',flush=True)
run('cross-build',['make','-j16','MILAN_FPGA='+str(fw),'O='+str(cross),
    'CC=aarch64-linux-gnu-gcc',str(cross/'milan-ctrld'),str(cross/'milan-dp'),str(cross/'milan-gptp-watch')],repo/'milan-linux')
