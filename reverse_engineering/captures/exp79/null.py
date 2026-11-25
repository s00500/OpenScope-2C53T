#!/usr/bin/env python3
"""Null measurement of GEN DC levels: Rigol offset centred on the level at
100 mV/div, reading corrected by the GND-coupled reading at the same setting
(that removes the offset DAC's own error)."""
import json, os, subprocess, time, sys
BWL = sys.argv[1] if len(sys.argv) > 1 else "off"
OS = os.environ.get("OPENSCOPE", "openscope")
def sh(*a):
    return subprocess.run(a, capture_output=True, text=True, timeout=30, check=True).stdout
def vavg():
    vals = []
    for _ in range(3):
        d = json.loads(sh("rigol", "measure", "vavg", "-c", "1", "--json").strip().splitlines()[-1])
        vals.append(d.get("CH1 vavg")); time.sleep(0.4)
    good = [v for v in vals if v is not None]
    return (sum(good) / len(good) if good else float("nan")), vals
sh("rigol", "acquire", "--type", "average", "--averages", "16")
sh("rigol", "timebase", "--scale", "1ms")
rows = []
for state, amp in [("off", None), ("dc", 0), ("dc", 1), ("dc", 5), ("dc", 10), ("dc", 15), ("dc", 20), ("dc", 25), ("dc", 30)]:
    out = sh(OS, "shell", "siggen off" if state == "off" else "siggen wave 9 amp %d on dump" % amp)
    code = 0 if state == "off" else [int(x) for x in out.splitlines()[0].split(":")[1].split()][0]
    est = 0.2769 + 0.00079169 * code if state != "off" else 0.25
    off = -round(est, 2)
    lvl = -off
    scale = "100mV" if lvl < 0.35 else "200mV" if lvl < 0.7 else "500mV" if lvl < 1.75 else "1V"
    sh("rigol", "channel", "1", "--scale", scale, "--offset=%.2fV" % off, "--coupling", "gnd", "--bwl", BWL)
    time.sleep(2.0); g, gv = vavg()
    sh("rigol", "channel", "1", "--coupling", "dc")
    time.sleep(2.0); d, dv = vavg()
    rows.append({"state": state, "amp_dv": amp, "code": code, "offset_v": off, "scale": scale, "gnd": gv, "dc": dv, "v": d - g})
    print("%-5s %-3s amp %4s code %4d  offset %+.2f  gnd %+.4f  dc %.4f  -> %.4f V   (3.3V*code/4095 = %.4f)"
          % (scale, state, amp, code, off, g, d, d - g, 3.3 * code / 4095))
json.dump(rows, open("null-bwl" + BWL + "-%s.json" % time.strftime("%H%M%S"), "w"), indent=1)
