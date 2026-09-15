#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: MIT
"""Build k3-am625x-myd-6254-tdm8.dtb from the vendor blob + the TDM8 sections.

The MYIR device tree is a binary the board ships with; mainline has no MYIR DTS
and the blob carries no __symbols__, so a .dtbo overlay cannot reference the
McASP node.  Instead we decompile the vendor blob, inject the sections from
k3-am625x-myd-6254-tdm8.dtsi at their target nodes with collision-free phandles,
and recompile.  The vendor blob itself is never touched.

  usage: mk-tdm8-dtb.py [base.dtb] [out.dtb] [--dtsi FILE] [--keep-dts]
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BSP = os.path.dirname(os.path.dirname(HERE))

PLACEHOLDERS = ("PH_PINS", "PH_MCASP1", "PH_CODEC", "PH_CODEC_DAI")


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def load_sections(path):
    """Split the .dtsi into {target-path: body} on '/* === SECTION: p === */'."""
    sections, target, buf = {}, None, []
    for line in open(path):
        m = re.match(r"\s*/\* === SECTION: (\S+) === \*/\s*$", line)
        if m:
            if target is not None:
                sections[target] = "".join(buf).rstrip("\n") + "\n"
            target, buf = m.group(1), []
        elif target is not None:
            buf.append(line)
    if target is not None:
        sections[target] = "".join(buf).rstrip("\n") + "\n"
    if not sections:
        sys.exit(f"{path}: no SECTION markers found")
    return sections


def max_phandle(dts):
    vals = [int(v, 0) for v in re.findall(r"\bphandle = <(0x[0-9a-fA-F]+|\d+)>;", dts)]
    return max(vals) if vals else 0


def prop_names(body):
    return {m.group(1) for m in re.finditer(r"^\s*([#\w,\.\-\+]+)\s*=", body, re.M)}


def node_spans(lines):
    """Map every node's absolute path to (open_line_idx, close_line_idx, depth)."""
    spans, stack = {}, []
    for i, line in enumerate(lines):
        s = line.strip()
        if s.endswith("{"):
            name = s[:-1].strip()
            if name.endswith("="):          # a multi-line property, not a node
                continue
            stack.append((name, i))
            path = "/" + "/".join(n for n, _ in stack[1:])
            spans.setdefault(path, [i, None, len(stack) - 1])
        elif s.startswith("};") and stack:
            name, open_i = stack.pop()
            path = "/" + "/".join(n for n, _ in stack) + ("/" + name if stack else "")
            path = path if stack else "/"
            for p, v in spans.items():
                if v[0] == open_i and v[1] is None:
                    v[1] = i
    return spans


def inject(dts, sections):
    lines = dts.splitlines(keepends=True)
    spans = node_spans(lines)

    # Apply deepest targets first so earlier edits never shift later line numbers.
    edits = []
    for target, body in sections.items():
        if target == "/":
            continue
        if target not in spans:
            sys.exit(f"target node not present in the vendor blob: {target}")
        open_i, close_i, depth = spans[target]
        if close_i is None:
            sys.exit(f"unterminated node: {target}")
        drop = prop_names(body)
        edits.append((open_i, close_i, depth, body, drop))

    for open_i, close_i, depth, body, drop in sorted(edits, reverse=True):
        # Delete the properties we are about to redefine, at this node's own level.
        keep, level = [], 0
        for i in range(open_i + 1, close_i):
            s = lines[i].strip()
            if level == 0:
                m = re.match(r"([#\w,\.\-\+]+)\s*=", s)
                if (m and m.group(1) in drop) or s.rstrip(";") in drop:
                    if s.endswith("{"):
                        level += 1
                    continue
            if s.endswith("{"):
                level += 1
            elif s.startswith("};"):
                level -= 1
            keep.append(lines[i])
        lines[open_i + 1:close_i] = [body] + keep

    if "/" in sections:
        for i in range(len(lines) - 1, -1, -1):
            if lines[i].strip() == "};":
                lines[i:i] = [sections["/"]]
                break
        else:
            sys.exit("could not find the root node's closing brace")

    return "".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("base", nargs="?",
                    default=os.path.join(BSP, "res/spare-sd/boot/ti/k3-am625x-myd-6254.dtb"))
    ap.add_argument("out", nargs="?",
                    default=os.path.join(BSP, "res/tdm8/k3-am625x-myd-6254-tdm8.dtb"))
    ap.add_argument("--dtsi", default=os.path.join(HERE, "k3-am625x-myd-6254-tdm8.dtsi"))
    ap.add_argument("--keep-dts", action="store_true",
                    help="also write the intermediate .dts next to the output")
    args = ap.parse_args()

    if not os.path.exists(args.base):
        sys.exit(f"base device tree not found: {args.base}\n"
                 "Pull the one the board actually boots:\n"
                 "  ssh board 'mount -o ro /dev/mmcblk1p1 /mnt/bootp; "
                 "cat /mnt/bootp/ti/k3-am625x-myd-6254.dtb; umount /mnt/bootp' > base.dtb")

    dts = subprocess.run(["dtc", "-q", "-I", "dtb", "-O", "dts", args.base],
                         check=True, capture_output=True, text=True).stdout

    base_max = max_phandle(dts)
    phandles = {name: base_max + 1 + i for i, name in enumerate(PLACEHOLDERS)}
    print(f"vendor blob's highest phandle: 0x{base_max:x}; allocating "
          + ", ".join(f"{k}=0x{v:x}" for k, v in phandles.items()))

    sections = load_sections(args.dtsi)
    for name, val in phandles.items():
        sections = {k: v.replace(f"@{name}@", f"0x{val:02x}") for k, v in sections.items()}
    left = {m for v in sections.values() for m in re.findall(r"@(\w+)@", v)}
    if left:
        sys.exit(f"unsubstituted placeholders: {sorted(left)}")

    merged = inject(dts, sections)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with tempfile.NamedTemporaryFile("w", suffix=".dts", delete=False) as f:
        f.write(merged)
        tmp = f.name
    try:
        # -q: the vendor blob decompiles without phandle type info, so dtc
        # emits a wall of gpios_property/phandle warnings that predate us.
        run(["dtc", "-q", "-I", "dts", "-O", "dtb", "-o", args.out, tmp])
    finally:
        if args.keep_dts:
            shutil.move(tmp, os.path.splitext(args.out)[0] + ".dts")
        else:
            os.unlink(tmp)

    print(f"wrote {args.out} ({os.path.getsize(args.out)} bytes)")


if __name__ == "__main__":
    main()
