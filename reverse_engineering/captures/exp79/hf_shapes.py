#!/usr/bin/env python3
import sys, time, json, re
sys.path.insert(0, ".")
import exp79 as E
rows = []
E.setup_rigol(tb="1us", acq="average", avg=64)
for f, tb in [(10000, "10us"), (25000, "5us"), (50000, "2us")]:
    E.rigol("timebase", "--scale", tb)
    for w in (0, 1):
        out = E.dev("siggen wave %d amp 30 duty 50 freq %d on dump" % (w, f))
        cyc = E.parse_cycle(out)
        actual = E.parse_actual_hz(out)
        E.rigol("trigger", "--level", "1.75V", "--slope", "rising")
        E.rigol("clear"); time.sleep(3.0)
        t, v = E.read_waveform("hf-%d-%d.csv" % (w, f))
        fit = E.zoh_fit(t, v, cyc, 1.0 / actual)
        span = (max(cyc) - min(cyc)) * fit["gain_v_per_code"]
        rows.append({"wave": w, "freq": f, "actual": actual, "fit": fit})
        print("%-6s %6d Hz  gain %.4f mV/code  rms %.1f mV (%.2f%% of span)  max %.0f mV"
              % ("sine" if w == 0 else "square", f, fit["gain_v_per_code"] * 1e3, fit["rms_v"] * 1e3,
                 100 * fit["rms_v"] / span, fit["max_abs_v"] * 1e3))
E.save("hfshapes", rows)
