#!/usr/bin/env python3.11
"""EXP-89: where is 0 V in ADC counts, per channel and range?

The input is SHORTED (probe tip on its ground clip, or a shorting cap), DC
coupling. For every range 0..9 and three offset-reference codes around the
boot value, capture the channel and record the mean, spread and railed
fraction. The middle code is the one the firmware sets at boot (DAC1 2048 for
CH1, TMR13 2544 for CH2), so that column is the zero in normal use; the other
two give the reference's sensitivity per range.

  python3.11 zero.py ch1           # CH1 shorted
  python3.11 zero.py ch2           # CH2 shorted (GEN cable off)
  python3.11 zero.py ch2 --gen     # CH2 with GEN at a DC code instead

Restores the range, coupling and offset code it found (the codes it found are
the boot ones; nothing reads them back, so it rewrites the boot value).
"""
import json
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "exp84"))
import loop  # noqa: E402

CH = sys.argv[1] if len(sys.argv) > 1 else "ch1"
N = 1 if CH == "ch1" else 2
BOOT = 2048 if N == 1 else 2544
CODES = (BOOT, BOOT - 512, BOOT + 512) if N == 1 else (BOOT, 2048, 3040)
CMD = "trig raw" if N == 1 else "trig2 raw"
FRAMES = 4

before = loop.dev_get_json()


def find(d, key):
    """The CLI's --json get: look the key up wherever it sits."""
    if isinstance(d, dict):
        if key in d:
            v = d[key]
            return v.get("value", v) if isinstance(v, dict) else v
        for v in d.values():
            r = find(v, key)
            if r is not None:
                return r
    elif isinstance(d, list):
        for v in d:
            if isinstance(v, dict) and v.get("key") == key:
                return v.get("value")
            r = find(v, key)
            if r is not None:
                return r
    return None


vdiv0 = find(before, "ch%d.vdiv" % N)
coup0 = find(before, "ch%d.coupling" % N)
print("found: ch%d.vdiv %s  coupling %s" % (N, vdiv0, coup0))

rows = []
try:
    loop.dev_set("ch%d.coupling" % N, "dc")
    for r in range(10):
        loop.dev_set("ch%d.vdiv" % N, str(r))
        for c in CODES:
            loop.shell("%s %d" % (CMD, c))
            time.sleep(1.2)
            xs = [x[m["skip"]:] for k, x, m in loop.capture(FRAMES, str(N)) if k[1] == "CH%d" % N]
            x = np.concatenate(xs)
            means = [float(v.mean()) for v in xs]
            row = {"ch": N, "range": r, "code": c, "mean": float(x.mean()),
                   "frame_sd": float(np.std(means)), "sd": float(x.std()),
                   "min": int(x.min()), "max": int(x.max()),
                   "railed": float(np.mean((x <= 0) | (x >= 255))), "n": int(x.size)}
            rows.append(row)
            print("r%d %s %4d  mean %7.2f  sd %5.2f  frames +-%.2f  [%3d..%3d]  railed %.0f%%"
                  % (r, CMD, c, row["mean"], row["sd"], row["frame_sd"], row["min"], row["max"],
                     100 * row["railed"]))
finally:
    loop.shell("%s %d" % (CMD, BOOT))
    if vdiv0 is not None:
        loop.dev_set("ch%d.vdiv" % N, str(vdiv0))
    if coup0 is not None:
        loop.dev_set("ch%d.coupling" % N, str(coup0))
    out = os.path.join(HERE, "zero-%s-%s.json" % (CH, time.strftime("%H%M%S")))
    json.dump({"argv": sys.argv, "boot_code": BOOT, "rows": rows}, open(out, "w"), indent=1)
    print("wrote", out)
