#!/usr/bin/env python3
"""Verify public packet hashes and exact tracked bytes/modes/index.
Usage: verify-evidence.py REPO PACKET
"""
import hashlib, json, os, stat, subprocess, sys
from pathlib import Path
repo, packet = map(lambda p: Path(p).resolve(), sys.argv[1:])
def git(*args):
    return subprocess.check_output(['git','-C',str(repo),*args])
head = 'afc350641dc2d93cee39bb4c62258b6d55f00ccb'
tree = '2930873abd5b2fdad7d11e72d0abc758222517d6'
assert git('rev-parse','HEAD').decode().strip() == head
assert git('rev-parse','HEAD^{tree}').decode().strip() == tree
assert git('write-tree').decode().strip() == tree
count = 0
for item in git('ls-tree','-rz',head).split(b'\0'):
    if not item: continue
    metadata, name = item.split(b'\t',1)
    mode, kind, oid = metadata.split()
    p = repo/os.fsdecode(name)
    if kind == b'commit':
        print('gitlink',os.fsdecode(name),oid.decode())
        continue
    data = os.fsencode(os.readlink(p)) if mode == b'120000' else p.read_bytes()
    got = hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
    assert got == oid.decode(), str(p)
    if mode != b'120000': assert bool(p.stat().st_mode & stat.S_IXUSR) == (mode == b'100755'), str(p)
    count += 1
assert not git('status','--porcelain','--untracked-files=no').strip()
print('PASS exact head/tree/index, tracked blobs and modes',count)
upstream = json.loads((packet/'scratch/upstream-tree.json').read_text())
assert upstream['truncated'] is False
links = {e['path']: e['sha'] for e in upstream['tree'] if e['mode'] == '160000'}
pin = dict(line.split('=',1) for line in (repo/'milan-linux/milan-fpga.pin').read_text().splitlines() if line and not line.startswith('#'))
for path,key in [('protocol-processor','PROTOCOL_PROCESSOR_REV'),('gptp-processor','GPTP_PROCESSOR_REV'),('third_party/tsn-c-stack','TSN_C_STACK_REV'),('third_party/lwSRP','LWSRP_REV')]:
    assert links[path] == pin[key]
    print('verified upstream gitlink',path,links[path])
hashfile = (repo/'br2-external/package/milan-fpga-src/milan-fpga-src.hash').read_text()
assert pin['MILAN_FPGA_SHA256'] in hashfile and pin['MILAN_FPGA_REV']+'.tar.gz' in hashfile
print('PASS Buildroot archive hash matches pin')
public = packet/'scratch/public/review-evidence/pb2aecp-r1'
manifest = json.loads((public/'MANIFEST.json').read_text())
n = 0
for entry in manifest:
    p = public/entry['file']
    assert hashlib.sha256(p.read_bytes()).hexdigest() == entry['published_sha256'], entry['file']
    n += 1
print('PASS published manifest hashes',n)
commit = git('cat-file','commit','cdfcd0bbb4bf5135947523ec1270b49f747ad1d6')
print('Evidence commit has embedded signature:', b'\ngpgsig ' in commit)
print('No detached signature listed:', not any('sig' in e['file'].split('/')[-1].lower() for e in manifest))
for name in ('milan-ctrld','milan-mediad','milan-dp','milan-gptp-watch'):
    p = packet/'scratch/cross'/name
    if p.exists(): print('rebuilt',name,hashlib.sha256(p.read_bytes()).hexdigest())
for name in ('milan-mediad','milan-dp','milan-gptp-watch'):
    p = public/'author-r3/receipts/aarch64'/name
    print('published',name,hashlib.sha256(p.read_bytes()).hexdigest())
print('Current config',hashlib.sha256((repo/'milan-linux/config/entity.conf').read_bytes()).hexdigest())
print('Published config',hashlib.sha256((public/'author-r3/receipts/aarch64/entity.conf').read_bytes()).hexdigest())
