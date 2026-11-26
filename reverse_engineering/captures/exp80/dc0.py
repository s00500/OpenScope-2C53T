#!/usr/bin/env python3
"""GEN DC transfer at Rigol offset 0 (zero good to ~3 mV after Self-Cal), two scales."""
import json, os, subprocess, time
OS = os.environ.get("OPENSCOPE", "openscope")
def sh(*a): return subprocess.run(a, capture_output=True, text=True, timeout=60, check=True).stdout
def vavg(n=3):
    v = []
    for _ in range(n):
        d = json.loads(sh("rigol", "measure", "vavg", "-c", "1", "--json").strip().splitlines()[-1])
        if d.get("CH1 vavg") is not None: v.append(d["CH1 vavg"])
        time.sleep(0.4)
    return sum(v) / len(v)
def fit(xs, ys):
    n = len(xs); mx = sum(xs)/n; my = sum(ys)/n
    a = sum((x-mx)*(y-my) for x, y in zip(xs, ys)) / sum((x-mx)**2 for x in xs); b = my - a*mx
    res = [y-(a*x+b) for x, y in zip(xs, ys)]
    return a, b, max(abs(r) for r in res)
sh("rigol", "acquire", "--type", "average", "--averages", "16"); sh("rigol", "timebase", "--scale", "1ms")
out = {}
for sc, amps in [("1V", range(0, 31)), ("500mV", range(0, 19))]:
    rows = []
    for bwl in ("20M", "off"):
        sh("rigol", "channel", "1", "--scale", sc, "--offset=0V", "--coupling", "gnd", "--bwl", bwl); time.sleep(2)
        z = vavg()
        sh("rigol", "channel", "1", "--coupling", "dc")
        for a in amps:
            o = sh(OS, "shell", "siggen wave 9 amp %d on dump" % a)
            code = int(o.splitlines()[0].split(":")[1].split()[0])
            time.sleep(1.2)
            rows.append({"bwl": bwl, "amp_dv": a, "code": code, "v": vavg(), "zero": z})
        pts = [(r["code"], r["v"]) for r in rows if r["bwl"] == bwl]
        a_, b_, mr = fit([p[0] for p in pts], [p[1] for p in pts])
        print("%-5s BW %-3s zero %+.4f  slope %.5f mV/code (full scale %.4f V)  intercept %+.4f V  max resid %.4f V"
              % (sc, bwl, z, a_*1e3, a_*4095, b_, mr))
        out.setdefault(sc, {})[bwl] = {"zero": z, "slope_v_per_code": a_, "intercept_v": b_, "max_resid_v": mr}
    out[sc]["rows"] = rows
sh(OS, "shell", "siggen off")
json.dump(out, open("dc0-%s.json" % time.strftime("%H%M%S"), "w"), indent=1)
