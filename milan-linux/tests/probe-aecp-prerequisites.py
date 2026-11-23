#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0
"""Reproduce the #29 generator/platform blockers without editing milan-fpga.

Usage: python3 -B tests/probe-aecp-prerequisites.py TREE OUTPUT

TREE is the unpacked 854bd723 archive with its protocol-processor and
gptp-processor dependencies. Requires PyYAML and a host C compiler (CC).
The inherited AX7101 fields isolate each refusal; NONE of these diagnostic
YAML files is a deployable configuration for the board. A zero exit means
the documented refusals and their positive control were reproduced, not
that the requested AECP integration passes.
"""

import copy
import os
from pathlib import Path
import shlex
import subprocess
import sys

import yaml


def main():
    tree, out = (Path(p).resolve() for p in sys.argv[1:])
    out.mkdir(parents=True, exist_ok=True)
    ctrl = tree / "sw/firmware/ctrl"
    base = yaml.safe_load((tree / "configs/endstation_ax7101_1x1_tdm8.yaml").read_text())
    base["entity"].update(name="PocketBeagle 2 bridge", entity_model_id="0x001BC50AC1000100",
                          serial_number="")
    for direction, label in (("talkers", "Out"), ("listeners", "In")):
        base["streams"][direction] = [dict(name=f"Stream {label} {i}", channels=8,
                                          map_mode="dynamic") for i in range(2)]
    base["audio_interface"]["word_length_bits"] = 32
    base["clocking"]["crf_output"]["enabled"] = True
    cases = {"two-aaf-with-crf": copy.deepcopy(base),
             "two-aaf-without-crf": copy.deepcopy(base),
             "two-aaf-out-one-aaf-in-with-crf": copy.deepcopy(base)}
    cases["two-aaf-without-crf"]["clocking"].update(
        crf_sink=False, crf_output={"enabled": False},
        media_clock_sources=["internal", "input_stream"])
    one = cases["two-aaf-out-one-aaf-in-with-crf"]
    one["streams"]["listeners"] = one["streams"]["listeners"][:1]
    one["clocking"]["crf_output"]["enabled"] = False
    cases["pb2-target"] = copy.deepcopy(one)
    cases["pb2-target"]["board"]["target"] = "pocketbeagle2"
    cases["host-gptp-owner"] = copy.deepcopy(one)
    cases["host-gptp-owner"]["board"]["features"]["fabric_gptp"] = False
    expected = {
        "two-aaf-with-crf": None,
        "two-aaf-without-crf": "2 AAF listener streams require a CRF Media Clock Output",
        "two-aaf-out-one-aaf-in-with-crf": None,
        "pb2-target": "board.target 'pocketbeagle2' not in ['arty', 'ax7101']",
        "host-gptp-owner": "board.features.fabric_gptp: false is retired",
    }

    def run(name, command, refusal=None, source=None):
        result = subprocess.run(command, input=source, capture_output=True, text=True, timeout=300)
        output = result.stdout + result.stderr
        log = f"COMMAND: {shlex.join(command)}\n"
        if source is not None:
            log += f"STDIN:\n{source}"
        log += f"EXIT: {result.returncode}\n{output}"
        (out / (name + ".log")).write_text(log)
        correct = result.returncode == 0 if refusal is None else (
            result.returncode != 0 and refusal in output)
        print(f"{name}: exit {result.returncode}; {'expected' if correct else 'UNEXPECTED'}")
        if not correct:
            raise RuntimeError(log)

    for name, cfg in cases.items():
        config = out / (name + ".yaml")
        config.write_text("# Diagnostic input only; inherited FPGA fields are not a board configuration.\n"
                          + yaml.safe_dump(cfg, sort_keys=False))
        run(name, [sys.executable, "-B", str(ctrl / "srp/srp_entity.py"), str(config),
                   "-o", str(out / (name + ".h"))], expected[name])
    for name in ("two-aaf-with-crf", "two-aaf-out-one-aaf-in-with-crf"):
        command = [*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-Werror", "-pedantic",
                   "-fsyntax-only", "-x", "c", "-include", str(out / (name + ".h")),
                   "-I" + str(ctrl / "loop"), "-I" + str(ctrl / "mbx"), "-"]
        run(name + "-compile", command,
            "THE SHAPES: the shape" if name == "two-aaf-with-crf" else None,
            '#include "ctrl_loop.h"\n')
    print("Prerequisite refusals reproduced; no daemon was built or tested.")


if __name__ == "__main__":
    main()
