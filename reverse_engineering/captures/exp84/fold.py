#!/usr/bin/env python3.11
"""EXP-81 addendum on CH2: (a) fold test, tones above Nyquist must alias to
|f - k fs| as the measured rate predicts; (b) second tones for 0x06..0x09."""
import json, math, os, sys, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import loop
FS = {0x06: 24999852.8, 0x07: 12499400.7, 0x08: 4999738.5, 0x09: 2499889.2,
      0x0E: 49997.3, 0x0F: 24998.6, 0x10: 12499.3, 0x11: 4999.7, 0x12: 2499.9}
PLAN = {0x0E: [30000, 37000, 45000], 0x0F: [14000, 19000, 31000, 47000],
        0x10: [7000, 9100, 11300, 23000], 0x11: [2900, 4100, 7300, 12100], 0x12: [1400, 2100, 3700, 6100],
        0x06: [30000], 0x07: [30000], 0x08: [50000], 0x09: [50000]}
rows = []
for code, tones in PLAN.items():
    loop.dev_set("timebase", "0x%02X" % code); time.sleep(1.0)
    fs = FS[code]
    for f in tones:
        fa = loop.gen("sine", f, 3.0); time.sleep(0.8)
        cps = [loop.fit_record(x[m["skip"]:])["cps"] for k, x, m in loop.capture(4, "2")]
        cps_med = float(np.median(cps))
        # predicted alias in cycles/sample: distance of f/fs to the nearest integer
        r = fa / fs; pred = abs(r - round(r))
        n = 896
        miss_bins = abs(cps_med - pred) * n
        fs_from_alias = None
        if f < fs / 2:
            fs_from_alias = fa / cps_med
        rows.append({"code": code, "gen_hz": fa, "fs_table": fs, "cps": cps, "pred_cps": pred, "miss_bins": miss_bins,
                     "fs_direct": fs_from_alias})
        tag = "fold" if f > fs / 2 else "tone"
        extra = (" fs %.1f (%+.0f ppm vs EXP-81)" % (fs_from_alias, (fs_from_alias / fs - 1) * 1e6)) if fs_from_alias else ""
        print("0x%02X %s gen %9.3f  cps %.6f  predicted %.6f  miss %.3f bins%s" % (code, tag, fa, cps_med, pred, miss_bins, extra))
json.dump(rows, open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "fold-%s.json" % time.strftime("%H%M%S")), "w"), indent=1)
