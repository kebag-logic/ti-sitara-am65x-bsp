#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: MIT

# Let U-Boot's binman run on a host whose setuptools no longer ships
# pkg_resources.
#
# U-Boot assembles tiboot3.bin / tispl.bin with binman, and binman <= v2025.04
# opens its own package data with pkg_resources:
#
#   tools/binman/control.py:  import pkg_resources
#                             pkg_resources.resource_string(__name__, 'missing-blob-help')
#                             pkg_resources.resource_listdir(__name__, 'etype')
#
# pkg_resources was a setuptools module, deprecated for years and REMOVED in
# setuptools 81. On a current distro (Arch ships setuptools 84 against Python
# 3.14) the import fails outright, even though setuptools itself is installed:
#
#   File ".../tools/binman/control.py", line 16, in <module>
#       import pkg_resources
#   ModuleNotFoundError: No module named 'pkg_resources'
#   make[1]: *** [Makefile:1135: .binman_stamp] Error 1
#
# and because build.sh has no `set -e`, the R5 and A53 builds then "finish"
# with no tiboot3.bin, tispl.bin or u-boot.img in the output directory at all.
#
# Upstream U-Boot moved these three call sites to importlib.resources; this
# script applies that same change. Only u-boot-pb (BeagleBoard's v2025.04 fork,
# the PocketBeagle 2 bootloader) needs it - u-boot-official is new enough to
# carry the fix already, which is why the SK and MYIR chains build fine.
#
# Installing setuptools<81 would also work, but that is a host-wide downgrade
# for one old tree. Patching the tree is the smaller blast radius.
#
# Idempotent, and a no-op on a tree that has no pkg_resources left.
#
# Usage: fix-binman-pkg-resources.sh [<u-boot-src>]   (default: ../../u-boot-pb)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
UB=${1:-$(cd "$HERE/../.." && pwd)/u-boot-pb}
C="$UB/tools/binman/control.py"

[ -f "$UB/Makefile" ] || { echo "not a U-Boot tree: $UB" >&2; exit 1; }
[ -f "$C" ] || { echo "no $C - nothing to fix" >&2; exit 0; }

if ! grep -q '^import pkg_resources$' "$C"; then
	echo "binman: no pkg_resources import - nothing to fix"
	exit 0
fi

python3 - "$C" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()

# 1. Bind the name unconditionally.  The v2025.04 block only binds
#    importlib_resources on the FALLBACK path, so on a modern Python - where
#    the try succeeds - the name does not exist at all and the replacements
#    below would NameError.  Upstream's block binds it either way, and also
#    catches AttributeError for the Python 3.8 that lacks .files().
old_import = """try:
    import importlib.resources
except ImportError:  # pragma: no cover
    # for Python 3.6
    import importlib_resources
"""
new_import = """try:
    import importlib.resources as importlib_resources
    # for Python 3.6, 3.7 and 3.8
    importlib_resources.files
except (ImportError, AttributeError): # pragma: no cover
    import importlib_resources
"""

# 2. Drop the dead import.
old_pkg = "import pkg_resources\n"

# 3. The two call sites, replaced exactly as upstream did.
old_help = ("    my_data = pkg_resources.resource_string("
            "__name__, 'missing-blob-help')\n")
new_help = ("    my_data = importlib_resources.files(__package__)"
            ".joinpath('missing-blob-help').read_bytes()\n")

old_glob = """    glob_list = pkg_resources.resource_listdir(__name__, 'etype')
    glob_list = [fname for fname in glob_list if fname.endswith('.py')]
"""
new_glob = """    entries = importlib_resources.files(__package__).joinpath('etype')
    glob_list = [entry.name for entry in entries.iterdir()
                 if entry.name.endswith('.py') and entry.is_file()]
"""

for label, old in (("import block", old_import), ("pkg_resources import", old_pkg),
                   ("missing-blob-help read", old_help), ("etype listing", old_glob)):
    if s.count(old) != 1:
        sys.exit(f"control.py: expected exactly one {label}, found {s.count(old)}.\n"
                 "This tree does not match U-Boot v2025.04 - check whether it "
                 "already carries the upstream importlib.resources fix and drop "
                 "this script.")

s = s.replace(old_import, new_import)
s = s.replace(old_pkg, "")
s = s.replace(old_help, new_help)
s = s.replace(old_glob, new_glob)
open(p, "w").write(s)
PY

# Prove it imports before handing the tree back to make, so a mistake here
# surfaces now rather than 40 seconds into the R5 build.
( cd "$UB/tools" && python3 -c "from binman import control; control.GetEntryModules()" ) \
	|| { echo "binman: still not importable after patching $C" >&2; exit 1; }

# binman's stamp file makes make skip the step that just failed
rm -f "$UB"/out_*/*/.binman_stamp 2>/dev/null || true

echo "binman: patched $C to use importlib.resources (pkg_resources is gone in setuptools >= 81)"
