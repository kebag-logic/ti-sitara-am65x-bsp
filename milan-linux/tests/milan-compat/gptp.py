#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0
"""gptp.py - flexptpd's status block, as tests/aecp.py plays it (a grandmaster one step away), until SIGTERM.

usage: gptp.py NAME ENTITY_ID_HEX
"""

import importlib.util
import signal
import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True
_spec = importlib.util.spec_from_file_location("aecp", Path(__file__).resolve().parent.parent / "aecp.py")
aecp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(aecp)

stop = False


def on_term(*_):
    global stop
    stop = True


signal.signal(signal.SIGTERM, on_term)
ptp = aecp.Flexptpd(sys.argv[1], int(sys.argv[2], 16))
while not stop:
    time.sleep(0.1)
ptp.close()
