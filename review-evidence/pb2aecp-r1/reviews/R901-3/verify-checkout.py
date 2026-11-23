#!/usr/bin/env python3
"""Verify exact source bytes, executable modes, index and dependency pins."""
import hashlib
import json
import pathlib
import subprocess
import sys
repo, packet = map(pathlib.Path, sys.argv[1:])
head = 'a5cd7c1e48019f89583e8df930299d457e016fe3'
tree = '5af6661a4d4492295704e0ceef2b263c1164bb70'
def git(*args, data=None):
    return subprocess.check_output(['git', '-C', str(repo), *args], input=data)
assert git('rev-parse', 'HEAD').decode().strip() == head
assert git('rev-parse', 'HEAD^{tree}').decode().strip() == tree
assert git('diff', '--cached', '--raw') == b''
count = 0
for entry in git('ls-tree', '-rz', 'HEAD').split(b'\0'):
    if not entry:
        continue
    meta, rawpath = entry.split(b'\t')
    mode, kind, oid = meta.decode().split()
    path = repo / rawpath.decode()
    if kind == 'commit':
        print('BSP gitlink', rawpath.decode(), oid, '(checkout initialized:', (path/'.git').exists(), ')')
        continue
    data = path.readlink().as_posix().encode() if path.is_symlink() else path.read_bytes()
    actual = git('hash-object', '--stdin', data=data).decode().strip()
    assert actual == oid, rawpath
    actual_mode = '120000' if path.is_symlink() else ('100755' if path.stat().st_mode & 0o111 else '100644')
    assert actual_mode == mode, rawpath
    count += 1
print('HEAD', head, 'tree', tree)
print('Verified tracked blob bytes and modes:', count, '; index matches HEAD')
print('Status:', git('status', '--porcelain=v1', '--untracked-files=all').decode() or 'clean')
pins = dict(line.split('=', 1) for line in (repo/'milan-linux/milan-fpga.pin').read_text().splitlines() if '=' in line and not line.startswith('#'))
mapping = {'protocol-processor':'PROTOCOL_PROCESSOR_REV','gptp-processor':'GPTP_PROCESSOR_REV','third_party/tsn-c-stack':'TSN_C_STACK_REV','third_party/lwSRP':'LWSRP_REV'}
for row in json.loads((packet/'upstream-gitlinks.json').read_text()):
    assert row['sha'] == pins[mapping[row['path']]]
    print('Verified upstream gitlink', row['path'], row['sha'])
assert (packet/'scratch/upstream/.milan-fpga-rev').read_text().strip() == pins['MILAN_FPGA_REV']
assert pins['MILAN_FPGA_SHA256'] in (repo/'br2-external/package/milan-fpga-src/milan-fpga-src.hash').read_text()
print('Upstream pin and package archive hash agree:', pins['MILAN_FPGA_REV'], pins['MILAN_FPGA_SHA256'])
for name in ['entity.conf','aecp_entity_gen.h','srp_entity_gen.h','nvm_shape_gen.h']:
    path = repo/'milan-linux/config'/name
    print(hashlib.sha256(path.read_bytes()).hexdigest(), 'config/'+name)
changed = git('diff', 'ed21f9b..HEAD', '--', 'milan-linux/config').decode()
print('Final re-pin config diff:\n'+changed)
