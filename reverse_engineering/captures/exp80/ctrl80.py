#!/usr/bin/env python3
"""EXP-80 controls: does the self-calibrated Rigol agree with itself?
1. GND coupling reads ~0 V at nonzero offsets (failed before Self-Cal: +39 mV, +157 mV).
2. The same GEN DC level reads the same with BW limit on and off (failed: 0.2 V / 1.36 V apart).
Run with GEN on the probe, nothing else changed."""
import json, os, subprocess, time
OS = os.environ.get("OPENSCOPE", "openscope")
def sh(*a): return subprocess.run(a, capture_output=True, text=True, timeout=60, check=True).stdout
def vavg():
    v = []
    for _ in range(3):
        d = json.loads(sh("rigol", "measure", "vavg", "-c", "1", "--json").strip().splitlines()[-1])
        v.append(d.get("CH1 vavg")); time.sleep(0.4)
    g = [x for x in v if x is not None]
    return sum(g) / len(g) if g else float("nan")
sh("rigol", "acquire", "--type", "average", "--averages", "16"); sh("rigol", "timebase", "--scale", "1ms")
rows = {"gnd": [], "bwl": []}
for sc, off in [("500mV", "-1.75V"), ("2V", "-1.4V"), ("1V", "-3.2V"), ("100mV", "0V"), ("500mV", "0V")]:
    sh("rigol", "channel", "1", "--scale", sc, "--offset=" + off, "--coupling", "gnd", "--bwl", "20M"); time.sleep(2)
    a = vavg()
    sh("rigol", "channel", "1", "--bwl", "off"); time.sleep(2)
    b = vavg()
    rows["gnd"].append({"scale": sc, "offset": off, "bwl20M": a, "bwloff": b})
    print("GND  %-5s %-6s  BW20M %+.4f  BWoff %+.4f" % (sc, off, a, b))
for amp, sc, off in [(0, "100mV", "-0.05V"), (5, "500mV", "-0.5V"), (15, "500mV", "-1.5V"), (30, "1V", "-3.0V")]:
    sh(OS, "shell", "siggen wave 9 amp %d on" % amp)
    sh("rigol", "channel", "1", "--scale", sc, "--offset=" + off, "--coupling", "dc", "--bwl", "20M"); time.sleep(2)
    a = vavg()
    sh("rigol", "channel", "1", "--bwl", "off"); time.sleep(2)
    b = vavg()
    rows["bwl"].append({"amp_dv": amp, "scale": sc, "offset": off, "bwl20M": a, "bwloff": b})
    print("GEN DC amp %2d  %-5s %-6s  BW20M %.4f  BWoff %.4f  diff %+.4f" % (amp, sc, off, a, b, a - b))
sh(OS, "shell", "siggen off")
json.dump(rows, open("ctrl80-%s.json" % time.strftime("%H%M%S"), "w"), indent=1)
