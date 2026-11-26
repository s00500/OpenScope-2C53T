#!/usr/bin/env python3.11
"""EXP-91: records in the state where the edge filter drops everything (CH1
off) and the control (CH1 on), same signal and settings otherwise.

  python3.11 grab.py <label> <ch1 on|off> [frames]
Writes <label>.csv (openscope capture) and <label>.json (state + counters)."""
import json, os, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
OS = os.environ.get("OPENSCOPE", "openscope")
def sh(*a, timeout=120):
    r = subprocess.run([OS, *a], capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError("%s: %s %s" % (a, r.stdout[-300:], r.stderr[-300:]))
    return r.stdout
label, ch1, frames = sys.argv[1], sys.argv[2], (sys.argv[3] if len(sys.argv) > 3 else "12")
sh("shell", "siggen wave 0 amp 30 duty 50 freq 1000 on")
for k, v in (("ch1.enabled", ch1), ("trig.source", "ch2"), ("trig.edge", "rising"),
             ("ch2.vdiv", "5"), ("timebase", "0x08"), ("trig.level", "19"), ("trig.mode", "auto")):
    sh("set", "--", k, v)
time.sleep(1.5)
ef0 = sh("shell", "fpga edgefilter")
sh("capture", "--frames", frames, "--channels", "1,2", "--out", os.path.join(HERE, label + ".csv"), "--wait", "60s")
ef1 = sh("shell", "fpga edgefilter")
st = json.loads(sh("--json", "get"))
json.dump({"label": label, "ch1": ch1, "edgefilter_before": ef0.strip(), "edgefilter_after": ef1.strip(),
           "state": {k: st[k] for k in ("ch1.enabled", "ch2.vdiv", "timebase", "trig.code", "trig.source", "trig.edge", "trig.mode")}},
          open(os.path.join(HERE, label + ".json"), "w"), indent=1)
print(label, ef0.strip(), "->", ef1.strip())
