#!/usr/bin/env python3
"""Verify published hashes and summarize selected public execution receipts."""
import hashlib
import json
import pathlib
import re
import subprocess
import sys
repo, packet = map(pathlib.Path, sys.argv[1:])
commit = '0aca227c4f85303d0a61439640d48e0c1bad08ee'
prefix = 'review-evidence/pb2aecp-r1/'
def git(*args):
    return subprocess.check_output(['git', '-C', str(repo), *args])
manifest = json.loads(git('show', f'{commit}:{prefix}MANIFEST.json'))
bad = []
for row in manifest:
    raw = git('show', f'{commit}:{prefix}{row["file"]}')
    if hashlib.sha256(raw).hexdigest() != row['published_sha256']:
        bad.append(row['file'])
print('Public manifest:', len(manifest), 'entries;', len(bad), 'mismatches', bad)
raw_commit = git('cat-file', 'commit', commit)
print('Embedded commit signature present:', b'\ngpgsig ' in raw_commit)
names = git('ls-tree', '-r', '--name-only', commit, prefix).decode().splitlines()
print('Detached signature paths:', [n for n in names if re.search(r'\.(sig|asc|minisig)$', n)])
root = packet / 'scratch/published' / prefix / 'author-r4/receipts'
for row in manifest:
    if row['file'].startswith('author-r4/receipts/'):
        target = packet / 'scratch/published' / prefix / row['file']
        if not target.exists():
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(git('show', f'{commit}:{prefix}{row["file"]}'))
for name in ['make-check-1.log','make-check-2.log','make-check-ad9c0f09-run1-listener-rate.log','make-check-ad9c0f09-run2-counters-capture.log','neg-counters-mutant-cache.log','neg-counters-mutant-drain.log','neg-nvm-mutant-carry.log','r901-edge-probes-head.log','r901-edge-probes-afc3506.log','r901-startup-probe-head.log','r901-startup-probe-afc3506.log','compat-default-options.log','compat-strict-options.log','cross-build-1.log','cross-build-2.log']:
    print('\nRECEIPT', name)
    lines = (root/'logs'/name).read_text().splitlines()
    for line in lines:
        if line.startswith(('cc ', 'aarch64-linux-gnu-gcc ', 'make ', 'make[')):
            continue
        if re.search(r'\[FAIL\]|FAIL|PASS \(|failed\)|RESULT|compatibility|Compatibility|Milan version|warnings:|errors:|b1254bb0|8bef6f76|c2824e61|elapsed|exit|rc[ =:]|binds|zeroed earlier|8109', line):
            print(line)
    print('TAIL', '\n'.join(lines[-3:]))
print('\nArchived binaries:')
for path in sorted((root/'aarch64').iterdir()):
    if path.is_file():
        print(hashlib.sha256(path.read_bytes()).hexdigest(), path.name)
assert not bad
