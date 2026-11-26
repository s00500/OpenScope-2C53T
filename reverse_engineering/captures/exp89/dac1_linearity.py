#!/usr/bin/env python3.11
"""EXP-89: is CH1's offset reference (DAC1) linear past 2560? GEN holds its
lowest DC level (code 20) into CH1, DC coupling; DAC1 2048..3072 in 256 steps
on ranges 5 and 9; the model predicts zero + 0.12108 x (code - 2048) plus a
constant (GEN's level / gain)."""
import json, os, sys, time
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "exp84"))
import loop
before = loop.dev_get_json()
rows = []
try:
    loop.dev_set("ch1.coupling", "dc")
    loop.gen("dc", 1, 0.0)
    for r in (5, 9):
        loop.dev_set("ch1.vdiv", str(r))
        for c in (2048, 2304, 2560, 2816, 3072):
            loop.shell("trig raw %d" % c)
            time.sleep(1.2)
            x = np.concatenate([x[m["skip"]:] for k, x, m in loop.capture(4, "1") if k[1] == "CH1"])
            rows.append({"range": r, "code": c, "mean": float(x.mean()), "max": int(x.max())})
        pts = [q for q in rows if q["range"] == r]
        c = np.array([q["code"] for q in pts], float); m = np.array([q["mean"] for q in pts])
        k, b = np.polyfit(c - 2048, m, 1)
        res = m - (b + k * (c - 2048))
        print("r%d  " % r + "  ".join("%d:%.2f" % (q["code"], q["mean"]) for q in pts)
              + "   slope %.5f  max|res| %.2f  vs model 0.12108: %+.2f counts at 3072"
              % (k, abs(res).max(), (k - 0.12108) * 1024))
finally:
    loop.shell("trig raw 2048")
    loop.shell("siggen wave 0 amp %d duty %d freq %d off" % (round(float(before["sg.amp"]) * 10),
               int(before["sg.duty"]), int(before["sg.freq"])))
    loop.dev_set("ch1.vdiv", str(before["ch1.vdiv"]))
    loop.dev_set("ch1.coupling", before["ch1.coupling"])
    json.dump(rows, open(os.path.join(HERE, "dac1-linearity-%s.json" % time.strftime("%H%M%S")), "w"), indent=1)
