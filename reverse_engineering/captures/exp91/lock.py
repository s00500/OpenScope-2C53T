#!/usr/bin/env python3.11
"""EXP-91: is the edge the FPGA fires on locked by the generator sharing the
acquisition loop's crystal? Edge-filter counters over 5 s per configuration
(CH1 on, CH2 = GEN sine, range 5, 0x08, level code 151, AUTO)."""
import json, os, re, subprocess, time
HERE = os.path.dirname(os.path.abspath(__file__))
OS = os.environ.get("OPENSCOPE", "openscope")
def sh(*a):
    r = subprocess.run([OS, *a], capture_output=True, text=True, timeout=60)
    if r.returncode: raise RuntimeError(r.stdout + r.stderr)
    return r.stdout
def counters():
    t = sh("shell", "fpga edgefilter")
    m = re.search(r"kept (\d+) dropped (\d+) unclassified (\d+)", t)
    return tuple(int(x) for x in m.groups())
rows = []
for freq, edge in ((1000, "rising"), (1000, "falling"), (1013, "rising"), (1013, "falling"), (997, "rising")):
    sh("shell", "siggen wave 0 amp 30 duty 50 freq %d on" % freq)
    for k, v in (("ch1.enabled", "on"), ("trig.source", "ch2"), ("trig.edge", edge), ("ch2.vdiv", "5"),
                 ("timebase", "0x08"), ("trig.level", "19"), ("trig.mode", "auto")):
        sh("set", "--", k, v)
    time.sleep(1.5)
    a = counters(); time.sleep(5.0); b = counters()
    d = [b[i] - a[i] for i in range(3)]
    rows.append({"hz": freq, "edge": edge, "kept": d[0], "dropped": d[1], "unknown": d[2]})
    print("%5d Hz  edge %-8s  kept %3d  dropped %3d  unclassified %3d" % (freq, edge, *d))
json.dump(rows, open(os.path.join(HERE, "lock-%s.json" % time.strftime("%H%M%S")), "w"), indent=1)
