#!/usr/bin/env python3.11
"""EXP-84: does TMR13/PA6 (`trig2 raw`) move CH2's level? Positive control:
DAC1 (`trig raw`) moves CH1's. GEN holds 1.0 V DC on CH2; CH1 is open."""
import json, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import loop
rows = []
def means():
    caps = loop.capture(3, "1,2")
    m = {"CH1": [], "CH2": []}
    for k, x, meta in caps:
        m[k[1]].append(float(x[meta["skip"]:].mean()))
    return {c: sum(v) / len(v) for c, v in m.items()}
for cmd, codes in (("trig raw", (2048, 1024, 3072, 2048)), ("trig2 raw", (1024, 2048, 3072, 2048))):
    for c in codes:
        print(loop.shell("%s %d" % (cmd, c)).strip().splitlines()[-1][:90])
        time.sleep(1.0)
        m = means()
        rows.append({"cmd": cmd, "code": c, **m})
        print("  %-9s %4d -> CH1 %.2f  CH2 %.2f" % (cmd, c, m["CH1"], m["CH2"]))
json.dump(rows, open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "offset-ref-%s.json" % time.strftime("%H%M%S")), "w"), indent=1)
