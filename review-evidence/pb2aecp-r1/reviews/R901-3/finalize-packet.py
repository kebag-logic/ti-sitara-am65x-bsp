#!/usr/bin/env python3
"""Collect public receipts and write a relative-path publication manifest."""
from pathlib import Path
import hashlib
import shutil
import sys
packet = Path(sys.argv[1]).resolve()
public = packet/'scratch/published/review-evidence/pb2aecp-r1'
target = packet/'public-author-r4'
target.mkdir(exist_ok=True)
shutil.copy2(public/'MANIFEST.json', packet/'published-MANIFEST.json')
receipts = public/'author-r4/receipts'
for part in ['logs','mutants']:
    shutil.copytree(receipts/part, target/part, dirs_exist_ok=True)
for name in ['INDEX.md','git-state.txt']:
    shutil.copy2(receipts/name, target/name)
shutil.copy2(receipts/'aarch64/SHA256SUMS', target/'original-SHA256SUMS')
report = (packet/'REPORT.md').read_text()
assert report.splitlines()[0] == '[R901] NEGATIVE - exact head a5cd7c1e48019f89583e8df930299d457e016fe3'
assert report.splitlines()[-1] == 'R901-3 FINISHED'
assert 'SKELETON' not in report
for name in ['fetch','make-check','cross-build','edge-probes','startup-probe','shape-reader']:
    assert (packet/(name+'.rc')).read_text().strip() == '0', name
files = sorted(p for p in packet.rglob('*') if p.is_file() and 'scratch' not in p.relative_to(packet).parts and p.name != 'MANIFEST.sha256')
with (packet/'MANIFEST.sha256').open('w') as out:
    for path in files:
        out.write(hashlib.sha256(path.read_bytes()).hexdigest()+'  '+path.relative_to(packet).as_posix()+'\n')
print('Publishable files:', len(files), '; scratch excluded')
