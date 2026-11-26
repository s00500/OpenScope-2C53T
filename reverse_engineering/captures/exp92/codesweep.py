#!/usr/bin/env python3.11
"""EXP-92: gains of the sensitive ranges (0..3) from GEN at raw DAC codes.

GEN (DAC2/PA5) by cable into the channel, DC coupling; `siggen dccode N`
sets one DAC code, 0.7937 mV each (EXP-80), so the levels can be a few
millivolts apart where `amp` moves 98 mV. Per range: codes 0 and 40 give
the slope, then 8 levels from code 0 up to about 200 counts; fit counts
against code x 0.7937 mV. The intercept less the EXP-89 zero (at the offset
code in force) is GEN's own 0 V, +7-8 mV on every range so far (EXP-89):
the cross-check.

  python3.11 codesweep.py ch2 [ranges...]     (default 3 2 1 0)
Restores the range, coupling and generator it found.
"""
import json, os, re, subprocess, sys, time
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "exp84"))
import loop  # noqa: E402

CH = sys.argv[1] if len(sys.argv) > 1 else "ch2"
N = 1 if CH == "ch1" else 2
RANGES = [int(a) for a in sys.argv[2:]] or [3, 2, 1, 0]
MV = 0.7937
CEIL = 226
TOP_COUNTS = 200.0
# EXP-89 zero model (scope_cal.c): zero_ref at code 2048 + k x (code - 2048)
ZREF = {1: [48.79, 46.00, 44.29, 43.89, 43.56, 48.83, 46.02, 44.66, 43.91, 43.61],
        2: [40.70, 42.54, 43.75, 44.23, 44.51, 40.80, 42.83, 43.93, 44.77, 44.93]}
KPC = {1: 0.12137, 2: 0.12195}


def offset_code():
    t = loop.shell("fpga scope zero")
    m = re.search(r"CH%d range \d+\s+(?:DAC1|TMR13) (\d+)" % N, t)
    return int(m.group(1))


def level(code):
    loop.shell("siggen dccode %d" % code)
    time.sleep(0.8)
    xs = [x[m["skip"]:] for k, x, m in loop.capture(3, str(N)) if k[1] == "CH%d" % N]
    x = np.concatenate(xs)
    return {"code": code, "mv": code * MV, "mean": float(x.mean()), "sd": float(x.std()),
            "min": int(x.min()), "max": int(x.max()),
            "clipped": bool(x.min() <= 0 or x.max() >= CEIL)}


before = loop.dev_get_json()
rows = []
try:
    loop.dev_set("ch%d.coupling" % N, "dc")
    loop.shell("siggen freq 1 dccode 0 on")
    oc = offset_code()
    print("offset code %d" % oc)
    for r in RANGES:
        loop.dev_set("ch%d.vdiv" % N, str(r))
        time.sleep(0.5)
        p0, p1 = level(0), level(40)
        per_code = (p1["mean"] - p0["mean"]) / 40.0
        pts = [p0, p1]
        if p0["clipped"] or per_code <= 0.01:
            print("r%d: code 0 %.1f, code 40 %.1f: no usable span" % (r, p0["mean"], p1["mean"]))
        else:
            top = int(min(4095, (TOP_COUNTS - p0["mean"]) / per_code))
            for c in sorted(set(int(round(t)) for t in np.linspace(0, top, 8)) - {0, 40}):
                pts.append(level(c))
        ok = [p for p in pts if not p["clipped"]]
        fit = None
        if len(ok) >= 3:
            v = np.array([p["mv"] for p in ok]); c = np.array([p["mean"] for p in ok])
            A = np.stack([np.ones_like(v), v], axis=1)
            (b0, b1), *_ = np.linalg.lstsq(A, c, rcond=None)
            res = c - (b0 + b1 * v)
            s2 = float(res @ res) / (len(ok) - 2)
            cov = s2 * np.linalg.inv(A.T @ A)
            zero = ZREF[N][r] + KPC[N] * (oc - 2048)
            fit = {"mv_per_count": float(1.0 / b1), "slope_se_rel": float(np.sqrt(cov[1, 1]) / b1),
                   "intercept": float(b0), "max_resid": float(np.abs(res).max()), "n": len(ok),
                   "zero_model": zero, "gen_offset_mv": float((b0 - zero) / b1),
                   "span_counts": float(c.max() - c.min()), "sd_max": float(max(p["sd"] for p in ok))}
            print("r%d  n=%d  %.4f mV/count (SE %.2f%%)  resid %.2f  span %.0f counts  sd<=%.2f  "
                  "intercept %.2f vs zero %.2f -> GEN offset %+.1f mV"
                  % (r, len(ok), fit["mv_per_count"], 100 * fit["slope_se_rel"], fit["max_resid"],
                     fit["span_counts"], fit["sd_max"], b0, zero, fit["gen_offset_mv"]))
        rows.append({"range": r, "points": pts, "fit": fit})
finally:
    w = {"sine": 0, "square": 1, "dc": 9}.get(before.get("sg.wave"), 0)
    loop.shell("siggen wave %d amp %d duty %d freq %d off" % (w, round(float(before["sg.amp"]) * 10),
               int(before["sg.duty"]), int(before["sg.freq"])))
    loop.dev_set("ch%d.vdiv" % N, str(before["ch%d.vdiv" % N]))
    loop.dev_set("ch%d.coupling" % N, str(before["ch%d.coupling" % N]))
    json.dump({"argv": sys.argv, "mv_per_code": MV, "rows": rows},
              open(os.path.join(HERE, "codesweep-%s-%s.json" % (CH, time.strftime("%H%M%S"))), "w"), indent=1)
