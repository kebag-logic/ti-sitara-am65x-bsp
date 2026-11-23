#!/usr/bin/env python3
"""Create and verify a relative SHA-256 publication manifest, excluding scratch."""
import hashlib
from pathlib import Path
import sys

packet = Path(sys.argv[1]).resolve()
report = (packet/'REPORT.md').read_text()
assert report.splitlines()[0] == '[R901] NEGATIVE - exact head f405a5754afbe30313116be4662adb9d79567f9e'
assert report.splitlines()[-1] == 'R901-1 FINISHED'
assert 'SKELETON' not in report
rows = []
for p in sorted(packet.rglob('*')):
    rel = p.relative_to(packet)
    if not p.is_file() or rel.parts[0] == 'scratch' or rel.as_posix() == 'MANIFEST.sha256':
        continue
    rows.append(hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + rel.as_posix())
(packet/'MANIFEST.sha256').write_text('\n'.join(rows)+'\n')
for row in rows:
    digest, name = row.split('  ',1)
    assert hashlib.sha256((packet/name).read_bytes()).hexdigest() == digest
print('PASS', len(rows), 'publishable files; relative paths; scratch excluded; report complete')
