#!/usr/bin/env python3.11
"""EXP-89: does the trigger fire at the volts the firmware prints?

GEN sine into CH1, DC, range 5, timebase 0x12 (2.5 kS/s, ~2 cycles per
record). One AUTO record gives the span; levels at 30/50/70 % of it; per
level NORMAL, 6 records. Time-ordered records put the trigger at index 512,
so r[512] must equal (reg 0x08 code - 28) (EXP-55/56: +-3 counts at 2 Hz;
here the sine is faster, see the tolerance), and the firmware's volts
(`fpga scope zero`) must equal (r[512] - zero) x gain.

Restores every setting it touches and saves.
"""
import json, os, re, sys, time
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "exp84"))
import loop

before = loop.dev_get_json()
keep = ["timebase", "trig.mode", "trig.level", "trig.source", "trig.edge", "ch1.coupling", "ch1.vdiv"]
print("found:", {k: before[k] for k in keep})
GAIN, F = 20.01, 5
out = {"levels": []}
try:
    loop.dev_set("ch1.coupling", "dc")
    loop.dev_set("ch1.vdiv", "5")
    loop.dev_set("trig.source", "ch1")
    loop.dev_set("trig.edge", "rising")
    loop.dev_set("timebase", "0x12")
    fa = loop.gen("sine", F, 2.5)
    loop.dev_set("trig.mode", "auto")
    time.sleep(2.0)
    caps = [x for k, x, m in loop.capture(2, "1") if k[1] == "CH1"]
    lo, hi = min(float(np.percentile(x, 2)) for x in caps), max(float(np.percentile(x, 98)) for x in caps)
    print("GEN %.3f Hz; span %.1f..%.1f counts" % (fa or 0, lo, hi))
    out.update(gen_hz=fa, lo=lo, hi=hi)
    for frac in (0.5, 0.3, 0.7):
        target = lo + frac * (hi - lo)
        lvl = int(round((target + 28 - 128) * 206 / 256))
        loop.dev_set("--", "trig.level", str(lvl))
        code = int(loop.dev_get_json()["trig.code"])
        z = loop.shell("fpga scope zero")
        mz = re.search(r"CH1 range 5\s+DAC1 (\d+)\s+zero (-?[\d.]+)", z)
        mv = re.search(r"mv (-?\d+)", z)
        zero, fw_mv = float(mz.group(2)), int(mv.group(1))
        loop.dev_set("trig.mode", "normal")
        time.sleep(2.0)
        recs = [(m, x) for k, x, m in loop.capture(6, "1") if k[1] == "CH1"]
        r512 = [int(x[512]) for m, x in recs]
        ordered = [m["ordered"] for m, x in recs]
        # local slope at 512, for the tolerance: counts per sample
        slope = [float(abs(x[516] - x[508]) / 8) for m, x in recs]
        med = float(np.median(r512))
        row = {"frac": frac, "level": lvl, "code": code, "want_record": code - 28, "r512": r512,
               "ordered": ordered, "slope": slope, "zero": zero, "fw_mv": fw_mv,
               "mv_from_r512": (med - zero) * GAIN}
        out["levels"].append(row)
        print("level %+d code %d -> record %d | r[512] %s (median %.0f, off %+.0f) ordered %s slope %.2f/sample"
              % (lvl, code, code - 28, r512, med, med - (code - 28), all(ordered), np.median(slope)))
        print("    firmware %+d mV; r[512] median as volts %+.0f mV (zero %.2f)" % (fw_mv, (med - zero) * GAIN, zero))
        loop.dev_set("trig.mode", "auto")
        time.sleep(0.5)
finally:
    loop.dev_set("trig.mode", "auto")
    for k in keep:
        v = before[k]
        if k == "timebase":
            v = "0x%02X" % int(v)
        loop.dev_set("--", k, str(v))
    loop.shell("siggen wave 0 amp %d duty %d freq %d off" % (round(float(before["sg.amp"]) * 10),
               int(before["sg.duty"]), int(before["sg.freq"])))
    loop.sh(loop.OS, "action", "save")
    after = loop.dev_get_json()
    print("restored:", {k: after[k] for k in keep}, "sg.output", after["sg.output"])
    json.dump(out, open(os.path.join(HERE, "trigcheck-%s.json" % time.strftime("%H%M%S")), "w"), indent=1)
