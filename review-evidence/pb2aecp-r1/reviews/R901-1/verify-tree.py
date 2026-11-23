#!/usr/bin/env python3
"""Verify exact bytes, modes, index and gitlinks without modifying the checkout.
Usage: verify-tree.py REPO PACKET
"""
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys

repo, packet = map(lambda p: Path(p).resolve(), sys.argv[1:])
head = 'f405a5754afbe30313116be4662adb9d79567f9e'
tree = '16e0c4c9fb07f3b837ab45fa227c66f65912a6c7'
def git(*args):
    return subprocess.check_output(['git', '-C', str(repo), *args])
assert git('rev-parse','HEAD').decode().strip() == head
assert git('rev-parse','HEAD^{tree}').decode().strip() == tree
expected = {}
for line in git('ls-tree','-rz',head).split(b'\0'):
    if line:
        fields, path = line.split(b'\t',1)
        mode, kind, oid = fields.decode().split()
        expected[os.fsdecode(path)] = (mode,kind,oid)
indexed = {}
for line in git('ls-files','--stage','-z').split(b'\0'):
    if line:
        fields,path=line.split(b'\t',1)
        mode,oid,stage=fields.decode().split()
        assert stage == '0'
        indexed[os.fsdecode(path)] = (mode,oid)
assert indexed == {p:(m,o) for p,(m,k,o) in expected.items()}
blobs = 0
for path,(mode,kind,oid) in expected.items():
    p=repo/path
    if mode=='160000':
        print('gitlink',path,oid)
        # An uninitialized gitlink is deliberately not initialized for this review.
        if (p/'.git').exists():
            got=subprocess.check_output(['git','-C',str(p),'rev-parse','HEAD']).decode().strip()
            assert got==oid
        else:
            assert p.is_dir() and not any(p.iterdir())
            print('submodule worktree uninitialized, empty; index/tree gitlink verified')
        continue
    st=p.lstat()
    if mode=='120000':
        assert stat.S_ISLNK(st.st_mode)
        data=os.fsencode(os.readlink(p))
    else:
        assert stat.S_ISREG(st.st_mode)
        assert ('100755' if st.st_mode & 0o111 else '100644') == mode, path
        data=p.read_bytes()
    actual=hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
    assert actual==oid,(path,actual,oid)
    blobs+=1
assert not git('status','--porcelain')
pin={}
for line in (repo/'milan-linux/milan-fpga.pin').read_text().splitlines():
    if line and not line.startswith('#') and '=' in line:
        key,value=line.split('=',1)
        pin[key]=value
entries=[]
for name in ['firmware-root','firmware-third-party']:
    response=json.loads((packet/'public'/f'{name}.json').read_text())
    entries.extend(json.loads(response['content']))
for path,key in [('protocol-processor','PROTOCOL_PROCESSOR_REV'),
                 ('gptp-processor','GPTP_PROCESSOR_REV'),
                 ('third_party/tsn-c-stack','TSN_C_STACK_REV'),
                 ('third_party/lwSRP','LWSRP_REV')]:
    entry=next(x for x in entries if x['path']==path)
    assert entry['sha']==pin[key],path
    print('upstream pinned gitlink',path,entry['sha'])
manifest=json.loads(json.loads((packet/'public/public-manifest.json').read_text())['content'])
for item in manifest:
    p=packet/'public'/Path(item['file']).name
    if p.exists():
        assert hashlib.sha256(p.read_bytes()).hexdigest()==item['published_sha256'],item['file']
        print('published receipt checksum verified',item['file'])
print('PASS exact head/tree, index and',blobs,'tracked blob bytes/modes; clean worktree')
