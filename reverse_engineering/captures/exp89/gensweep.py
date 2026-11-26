#!/usr/bin/env python3.11
"""EXP-89: GEN DC levels into one channel, every range: gain AND intercept.

GEN (DAC2/PA5) by cable into the channel, DC coupling. GEN's volts are
0.7937 mV x code + b, b = -6..+35 mV (EXP-80, self-calibrated Rigol). Per
range: two probe levels (0.0 and 0.1 V settings) estimate counts per volt,
then up to six levels below the rail are measured and fitted:

    counts = zero + V / gain,   V = 0.7937 mV x code

The slope is the gain (compare scope_cal.c); the intercept is the zero plus
GEN's own offset b / gain, so against a shorted-input zero it measures b,
which must then be the same on every range.

  python3.11 gensweep.py ch2 [offset_code]   # default: CH2 at TMR13 2048 (EXP-84)
  python3.11 gensweep.py ch1 [offset_code]   # default: CH1 at DAC1 2048
"""
import json
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "exp84"))
import loop  # noqa: E402

CH = sys.argv[1] if len(sys.argv) > 1 else "ch2"
N = 1 if CH == "ch1" else 2
OFF = int(sys.argv[2]) if len(sys.argv) > 2 else 2048
BOOT = 2048 if N == 1 else 2544
CMD = "trig raw" if N == 1 else "trig2 raw"
MV_PER_CODE = 0.7937
RAIL_SAFE = 200.0
CEIL = 226              # CH2 saturates at 227 counts (EXP-89), not 255
RANGES = [int(a) for a in sys.argv[3:]] or list(range(10))


def gen_code(n_tenths):
    return 20 + n_tenths * 4095 // 33           # firmware's DC mapping (EXP-80)


def level(n_tenths):
    loop.gen("dc", 1, n_tenths / 10.0)
    time.sleep(1.0)
    xs = [x[m["skip"]:] for k, x, m in loop.capture(3, str(N)) if k[1] == "CH%d" % N]
    x = np.concatenate(xs)
    return {"tenths": n_tenths, "code": gen_code(n_tenths), "v": gen_code(n_tenths) * MV_PER_CODE / 1000,
            "mean": float(x.mean()), "sd": float(x.std()), "min": int(x.min()), "max": int(x.max()),
            "railed": float(np.mean((x <= 0) | (x >= 255))),
            "clipped": bool(x.min() <= 0 or x.max() >= CEIL)}


before = loop.dev_get_json()
vdiv0, coup0 = before.get("ch%d.vdiv" % N), before.get("ch%d.coupling" % N)
print("found: ch%d.vdiv %s coupling %s; offset ref -> %d" % (N, vdiv0, coup0, OFF))
rows = []
try:
    loop.dev_set("ch%d.coupling" % N, "dc")
    loop.shell("%s %d" % (CMD, OFF))
    for r in RANGES:
        loop.dev_set("ch%d.vdiv" % N, str(r))
        p0, p1 = level(0), level(1)
        pts = [p0, p1]
        per_tenth = p1["mean"] - p0["mean"]
        if p0["clipped"] or p1["clipped"] or per_tenth <= 0.0:
            print("r%d  code20 %.2f  0.1V %.2f  -> no usable span" % (r, p0["mean"], p1["mean"]))
        else:
            top = min(30, int((RAIL_SAFE - p0["mean"]) / max(per_tenth, 1e-3)))
            want = sorted(set(int(round(t)) for t in np.linspace(0, top, 7)) - {0, 1})
            pts += [level(t) for t in want]
        ok = [p for p in pts if not p["clipped"]]
        fit = None
        if len(ok) >= 2:
            v = np.array([p["v"] for p in ok]); c = np.array([p["mean"] for p in ok])
            A = np.stack([np.ones_like(v), v], axis=1)
            (b0, b1), res, *_ = np.linalg.lstsq(A, c, rcond=None)
            resid = c - (b0 + b1 * v)
            se = None
            if len(ok) > 2:
                s2 = float(resid @ resid) / (len(ok) - 2)
                cov = s2 * np.linalg.inv(A.T @ A)
                se = {"intercept": float(np.sqrt(cov[0, 0])), "slope_rel": float(np.sqrt(cov[1, 1]) / b1)}
            fit = {"intercept": float(b0), "counts_per_v": float(b1), "mv_per_count": float(1000 / b1),
                   "max_resid": float(np.max(np.abs(resid))), "n": len(ok), "se": se}
            print("r%d  n=%d  %.2f mV/count  intercept %.2f counts  max resid %.2f  span %.0f counts"
                  % (r, len(ok), fit["mv_per_count"], b0, fit["max_resid"], c.max() - c.min()))
        rows.append({"range": r, "points": pts, "fit": fit})
finally:
    w = {"sine": 0, "square": 1, "dc": 9}.get(before.get("sg.wave"))
    if w is not None:
        loop.shell("siggen wave %d amp %d duty %d freq %d off"
                   % (w, round(float(before.get("sg.amp", 3)) * 10), int(before.get("sg.duty", 50)),
                      int(before.get("sg.freq", 1000))))
    loop.shell("siggen off")
    loop.shell("%s %d" % (CMD, BOOT))
    if vdiv0 is not None:
        loop.dev_set("ch%d.vdiv" % N, str(vdiv0))
    if coup0 is not None:
        loop.dev_set("ch%d.coupling" % N, str(coup0))
    out = os.path.join(HERE, "gensweep-%s-%s.json" % (CH, time.strftime("%H%M%S")))
    json.dump({"argv": sys.argv, "offset_code": OFF, "mv_per_code": MV_PER_CODE, "rows": rows},
              open(out, "w"), indent=1)
    print("wrote", out)
