#!/usr/bin/env python3
"""EXP-79 harness: the 2C53T generator (GEN jack) against the Rigol DS1104Z.

The device is driven through `openscope shell "siggen ..."` (works in any
mode); the Rigol through the `rigol` CLI. Every run writes its raw numbers to
a JSON file next to this script.

  python3 exp79.py freq     # TMR7 frequency vs the Rigol hardware counter
  python3 exp79.py dc       # DC transfer: DAC code -> volts at GEN
  python3 exp79.py shapes   # 13 waveforms vs the device's own 100-sample cycle
  python3 exp79.py duty     # square duty vs the Rigol's +duty
  python3 exp79.py bw       # sine amplitude vs frequency
"""
import json
import os
import re
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
OPENSCOPE = os.environ.get("OPENSCOPE", "openscope")
FULL_SCALE = 4095.0


def sh(args, timeout=30):
    r = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError("%s failed: %s %s" % (args[0], r.stdout[-400:], r.stderr[-400:]))
    return r.stdout


def dev(cmd):
    return sh([OPENSCOPE, "shell", cmd])


def rigol(*args):
    return sh(["rigol", *args])


def scpi_float(query):
    out = rigol("scpi", query).strip().splitlines()[-1]
    return float(out)


def measure(items, n=1):
    """Rigol automatic measurements on CH1, `n` readings 0.4 s apart."""
    rows = []
    for i in range(n):
        out = rigol("measure", *items, "-c", "1", "--json").strip().splitlines()[-1]
        d = json.loads(out)
        rows.append({k.split(" ", 1)[1]: v for k, v in d.items() if k.startswith("CH1 ")})
        if i + 1 < n:
            time.sleep(0.4)
    return rows


def parse_actual_hz(text):
    m = re.search(r"-> (\d+)\.(\d{3}) Hz", text)
    return int(m.group(1)) + int(m.group(2)) / 1000.0 if m else None


def parse_cycle(text):
    vals = []
    for line in text.splitlines():
        m = re.match(r"\s*(\d+):((?:\s+\d+)+)\s*$", line)
        if m:
            vals.extend(int(x) for x in m.group(2).split())
    return vals[:100] if len(vals) >= 100 else None


def setup_rigol(scale="500mV", offset="-1.75V", tb="500us", acq="normal", avg=None,
                level="1.5V", slope="rising"):
    rigol("channel", "1", "--scale", scale, "--offset", offset, "--bwl", "20M", "--coupling", "dc")
    rigol("timebase", "--scale", tb, "--offset", "0")
    rigol("trigger", "--mode", "edge", "--source", "1", "--sweep", "auto", "--coupling",
          "hfreject", "--level", level, "--slope", slope)
    if acq == "average":
        rigol("acquire", "--type", "average", "--averages", str(avg or 16))
    else:
        rigol("acquire", "--type", "normal")


def save(name, data):
    path = os.path.join(HERE, "%s-%s.json" % (name, time.strftime("%H%M%S")))
    with open(path, "w") as f:
        json.dump(data, f, indent=1)
    print("saved", path)


def linfit(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    a = sxy / sxx
    b = my - a * mx
    res = [y - (a * x + b) for x, y in zip(xs, ys)]
    ss_tot = sum((y - my) ** 2 for y in ys)
    r2 = 1 - sum(r * r for r in res) / ss_tot if ss_tot else float("nan")
    return a, b, res, r2


# ── frequency ────────────────────────────────────────────────────────────
FREQS = [1, 2, 5, 10, 20, 37, 50, 100, 200, 500, 1000, 1234, 2000, 5000,
         10000, 12345, 20000, 25000, 33333, 40000, 49999, 50000]


def run_freq():
    setup_rigol(tb="500us")
    rigol("scpi", ":MEAS:COUN:SOUR CHAN1")
    rows = []
    for f in FREQS:
        out = dev("siggen wave 0 amp 30 duty 50 freq %d on" % f)
        actual = parse_actual_hz(out)
        settle = max(2.0, 4.0 / f + 1.5)
        time.sleep(settle)
        reads = []
        for _ in range(4):
            reads.append(scpi_float(":MEAS:COUN:VAL?"))
            time.sleep(max(0.7, 2.0 / f))
        med = statistics.median(reads)
        ppm = (med / actual - 1.0) * 1e6 if actual else None
        rows.append({"req_hz": f, "device_hz": actual, "counter_hz": reads,
                     "counter_median": med, "ppm_vs_device": ppm,
                     "ppm_vs_request": (med / f - 1.0) * 1e6})
        print("%6d Hz  device %12.3f  counter %14.6f  %+9.1f ppm (vs req %+9.1f)  spread %.3g"
              % (f, actual or 0, med, ppm or 0, rows[-1]["ppm_vs_request"],
                 max(reads) - min(reads)))
    save("freq", rows)


# ── DC transfer ──────────────────────────────────────────────────────────
def run_dc():
    setup_rigol(tb="1ms", acq="average", avg=16)
    rows = []
    out = dev("siggen off")
    time.sleep(2.0)
    m = measure(["vavg", "vmax", "vmin"], n=3)
    rows.append({"state": "off", "code": None, "rigol": m})
    print("off            vavg %s" % [r["vavg"] for r in m])
    for a in range(0, 31):
        out = dev("siggen wave 9 amp %d on dump" % a)
        cyc = parse_cycle(out)
        code = cyc[0] if cyc else None
        flat = cyc is not None and min(cyc) == max(cyc)
        time.sleep(1.5)
        m = measure(["vavg", "vmax", "vmin"], n=3)
        vav = statistics.mean(r["vavg"] for r in m)
        rows.append({"state": "dc", "amp_dv": a, "code": code, "flat": flat, "rigol": m,
                     "vavg_mean": vav})
        print("amp %2d  code %4s  vavg %.4f V  (expect %.4f at 3.3 V ref)"
              % (a, code, vav, (code or 0) * 3.3 / FULL_SCALE))
    pts = [(r["code"], r["vavg_mean"]) for r in rows if r.get("state") == "dc" and r["code"] is not None]
    a, b, res, r2 = linfit([p[0] for p in pts], [p[1] for p in pts])
    fit = {"volts_per_code": a, "intercept_v": b, "implied_ref_v": a * FULL_SCALE,
           "r2": r2, "max_abs_residual_v": max(abs(x) for x in res)}
    print("fit: %.6f mV/code, intercept %.4f V, implied full scale %.4f V, R2 %.6f, max resid %.4f V"
          % (a * 1e3, b, a * FULL_SCALE, r2, fit["max_abs_residual_v"]))
    save("dc", {"rows": rows, "fit": fit})


# ── shapes ───────────────────────────────────────────────────────────────
def read_waveform(name):
    path = os.path.join(HERE, name)
    rigol("waveform", "-c", "1", "-o", path)
    t, v = [], []
    with open(path) as f:
        next(f)
        for line in f:
            a, b = line.strip().split(",")
            t.append(float(a))
            v.append(float(b))
    return t, v


def zoh_fit(t, v, cyc, period):
    """Best phase, then least-squares gain/offset of the ZOH cycle against v."""
    n = len(cyc)
    best = None
    step = period / n / 10.0
    for k in range(n * 10):
        t0 = k * step
        model = [cyc[int(((ti - t0) % period) / period * n) % n] for ti in t]
        a, b, res, r2 = linfit(model, v)
        rms = (sum(r * r for r in res) / len(res)) ** 0.5
        if best is None or rms < best["rms_v"]:
            best = {"t0_s": t0, "gain_v_per_code": a, "offset_v": b, "rms_v": rms, "r2": r2,
                    "max_abs_v": max(abs(r) for r in res)}
    return best


def run_shapes(freq=1000, avg=64):
    rows = []
    setup_rigol(tb="100us", acq="average", avg=avg)
    for w in range(13):
        out = dev("siggen wave %d amp 30 duty 50 freq %d on dump" % (w, freq))
        cyc = parse_cycle(out)
        name = re.search(r"wave=\d+\(([^)]*)\)", out).group(1)
        if cyc is None or min(cyc) == max(cyc):
            rows.append({"wave": w, "name": name, "skipped": "flat"})
            print("%2d %-11s flat, skipped" % (w, name))
            continue
        mid_v = (min(cyc) + max(cyc)) / 2 * 3.3 / FULL_SCALE
        rigol("trigger", "--level", "%.2fV" % mid_v, "--slope", "rising")
        rigol("clear")
        time.sleep(3.0)
        t, v = read_waveform("shape-%02d.csv" % w)
        fit = zoh_fit(t, v, cyc, 1.0 / freq)
        span = (max(cyc) - min(cyc)) * fit["gain_v_per_code"]
        rows.append({"wave": w, "name": name, "cycle": cyc, "fit": fit, "span_v": span})
        print("%2d %-11s gain %.4f mV/code  offset %+.3f V  rms %.1f mV (%.2f%% of span)  max %.0f mV"
              % (w, name, fit["gain_v_per_code"] * 1e3, fit["offset_v"], fit["rms_v"] * 1e3,
                 100 * fit["rms_v"] / span if span else 0, fit["max_abs_v"] * 1e3))
    save("shapes", rows)


# ── duty ─────────────────────────────────────────────────────────────────
def run_duty():
    setup_rigol(tb="200us", acq="average", avg=16)
    rows = []
    for d in range(0, 101, 5):
        out = dev("siggen wave 1 amp 30 freq 1000 duty %d on dump" % d)
        cyc = parse_cycle(out)
        hi = max(cyc)
        expect = sum(1 for c in cyc if c == hi) / len(cyc) if min(cyc) != hi else (1.0 if hi > 1000 else 0.0)
        rigol("clear")
        time.sleep(2.0)
        m = measure(["pduty", "freq", "vmax", "vmin"], n=3)
        got = [r.get("pduty") for r in m]
        rows.append({"duty_set": d, "expect_from_cycle": expect, "rigol": m})
        print("duty %3d  cycle says %.2f  rigol pduty %s" % (d, expect, got))
    save("duty", rows)


# ── bandwidth ────────────────────────────────────────────────────────────
TB_STEPS = [1e-6, 2e-6, 5e-6, 10e-6, 20e-6, 50e-6, 100e-6, 200e-6, 500e-6, 1e-3, 2e-3,
            5e-3, 10e-3, 20e-3, 50e-3, 100e-3, 200e-3]


def tb_for(f, cycles=4.0):
    want = cycles / f / 12.0
    for s in TB_STEPS:
        if s >= want:
            return s
    return TB_STEPS[-1]


def fmt_s(s):
    if s >= 1e-3:
        return "%gms" % (s * 1e3)
    return "%gus" % (s * 1e6)


def run_bw():
    setup_rigol(acq="average", avg=16)
    rows = []
    for f in [10, 100, 1000, 2000, 5000, 10000, 15000, 20000, 25000, 30000, 40000, 50000]:
        dev("siggen wave 0 amp 30 duty 50 freq %d on" % f)
        rigol("timebase", "--scale", fmt_s(tb_for(f)))
        rigol("clear")
        time.sleep(max(2.5, 16 * 12 * tb_for(f) * 3))
        m = measure(["vpp", "vmax", "vmin", "vrms", "freq"], n=3)
        vpp = statistics.median(r["vpp"] for r in m)
        rows.append({"req_hz": f, "timebase_s": tb_for(f), "rigol": m, "vpp_median": vpp})
        print("%6d Hz  tb %-6s  vpp %.3f  vmax %.3f  vmin %.3f"
              % (f, fmt_s(tb_for(f)), vpp, m[-1]["vmax"], m[-1]["vmin"]))
    save("bw", rows)


if __name__ == "__main__":
    which = sys.argv[1] if len(sys.argv) > 1 else ""
    {"freq": run_freq, "dc": run_dc, "shapes": run_shapes, "duty": run_duty, "bw": run_bw}[which]()
