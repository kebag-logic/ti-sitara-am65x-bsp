#!/usr/bin/env python3
"""Create and verify a publication manifest; scratch is never included."""
from pathlib import Path
import hashlib
p = Path(__file__).resolve().parent
report = (p/'REPORT.md').read_text()
assert report.splitlines()[0] == '[R901] NEGATIVE - exact head afc350641dc2d93cee39bb4c62258b6d55f00ccb'
assert report.splitlines()[-1] == 'R901-2 FINISHED'
assert 'SKELETON' not in report
for name, expected in {'make-check':0,'check-compat':0,'cross-build':0,'media-cross':0,'integrity':0,'gptp-stale-test':0,'identity-pristine-distinct-test':0,'identity-mutant-original-test':1,'identity-mutant-distinct-test':1,'edge-probes':1,'startup-probe':1}.items():
    assert int((p/(name+'.rc')).read_text()) == expected, name
files = sorted(f for f in p.iterdir() if f.is_file() and f.name != 'MANIFEST.sha256')
lines = [hashlib.sha256(f.read_bytes()).hexdigest()+'  '+f.name for f in files]
(p/'MANIFEST.sha256').write_text('\n'.join(lines)+'\n')
for line in lines:
    digest, name = line.split('  ',1)
    assert hashlib.sha256((p/name).read_bytes()).hexdigest() == digest
print('Final report and',len(lines),'publication files verified; scratch excluded.')
