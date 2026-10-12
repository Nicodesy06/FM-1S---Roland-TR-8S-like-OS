#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PULSAR-8 contributors
"""Level calibration: reads host/audio_test's per-instrument peaks (stdin) and rewrites the
instrument trims in firmware/src/pz_dsp.c so every category plays at its target peak."""
import re
import sys
from pathlib import Path

DSP = Path(__file__).resolve().parents[1] / "firmware" / "src" / "pz_dsp.c"
TARGET = {'CAT_BD': 22000, 'CAT_SD': 20000, 'CAT_TOM': 19000, 'CAT_RS': 15000, 'CAT_CP': 16000,
          'CAT_HH': 12000, 'CAT_OH': 12000, 'CAT_CY': 11000, 'CAT_PC': 15000}
src = DSP.read_text()
insts = re.findall(r'\[(I_\w+)\] = (SY|PC)\("([^"]+)", "(\w+)", (CAT_\w+), (\w+),', src)
byname = {n: (k, e, c) for _, k, n, _, c, e in insts}
changed = 0
for line in sys.stdin:
    m = re.match(r'(.{11})\s+(\d+)\s+(\d+)\s+\d+', line)
    if not m or m.group(1).strip() not in byname:
        continue
    name, peak = m.group(1).strip(), int(m.group(2))
    k, e, c = byname[name]
    f = TARGET[c] / max(peak, 1)
    if k == 'SY':
        mm = re.search(r'\[' + e + r'\] = \{[^}]*?\.lvl = (\d+)', src)
    else:
        mm = re.search(r'\[' + e + r'\] = \{SMP_\w+, MA_\w+, -?\d+, \d+, (\d+)\}', src)
    old = int(mm.group(1))
    new = max(4, min(255, round(old * f)))
    if new != old:
        changed += 1
        src = src[:mm.start(1)] + str(new) + src[mm.end(1):]
    if new == 255:
        print(f"{name}: at the trim limit")
DSP.write_text(src)
print(f"level_insts: {changed} trims changed")
