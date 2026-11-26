#!/usr/bin/env python3.11
"""Loopback harness: the 2C53T's own generator (GEN) into its own CH1.

The generator's real frequency (`sg.freq.actual`) is exact to the crystal
(EXP-79), so a tone fitted in a captured record gives the sample rate:
fs = f_actual / (cycles per sample).

  python3.11 loop.py control            # GEN -> CH1 is wired: tone visible, CH2 crosstalk
  python3.11 loop.py rates [codes...]   # sample rate per reg-0x01 code
  python3.11 loop.py gain [ranges...]   # counts per GEN code per range (CH1)
  python3.11 loop.py badge              # the device's own frequency estimate vs the truth

Records can be a rotation of one continuous segment (a seam where the
FPGA's write pointer was). The fit finds the seam from the residual of a
whole-record fit and refits the two segments with one shared frequency.
"""
import csv
import json
import math
import os
import subprocess
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OS = os.environ.get("OPENSCOPE", "openscope")


def sh(*a, timeout=60):
    r = subprocess.run(a, capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError("%s: %s %s" % (" ".join(a), r.stdout[-300:], r.stderr[-300:]))
    return r.stdout


def dev_set(*kv):
    return sh(OS, "set", *kv)


def dev_get_json():
    return json.loads(sh(OS, "--json", "get"))


def shell(cmd):
    return sh(OS, "shell", cmd)


def gen(wave="sine", freq=1000, amp=3.0, duty=50, on=True):
    """Drive GEN through the shell (works in scope mode); return the real Hz."""
    waves = {"sine": 0, "square": 1, "dc": 9}
    out = shell("siggen wave %d amp %d duty %d freq %d %s"
                % (waves[wave], round(amp * 10), duty, freq, "on" if on else "off"))
    for line in out.splitlines():
        if "-> " in line and " Hz" in line:
            v = line.split("-> ")[1].split(" Hz")[0]
            return float(v)
    return None


def capture(frames, channels="1", path=None):
    path = path or os.path.join(HERE, "cap.csv")
    sh(OS, "capture", "--frames", str(frames), "--channels", channels, "--out", path,
       "--wait", "60s", timeout=120)
    recs = {}
    meta = {}
    with open(path) as f:
        for r in csv.DictReader(f):
            k = (int(r["frame_id"]), r["channel"])
            recs.setdefault(k, []).append(int(r["count"]))
            meta[k] = {"skip": int(r["head_skip"]), "ordered": r["time_ordered"] == "true",
                       "fs": float(r["sample_rate_hz"])}
    return [(k, np.array(v, dtype=float), meta[k]) for k, v in sorted(recs.items())]


# ── sine fitting ─────────────────────────────────────────────────────────
def fit_fixed(x, w, segs):
    """Least squares for amplitude/phase per segment, one offset per segment,
    at a fixed angular frequency w (rad/sample). Returns rss, params."""
    n = len(x)
    t = np.arange(n, dtype=float)
    cols = []
    for (a, b) in segs:
        m = np.zeros(n)
        m[a:b] = 1.0
        cols += [m * np.cos(w * t), m * np.sin(w * t), m]
    A = np.stack(cols, axis=1)
    p, *_ = np.linalg.lstsq(A, x, rcond=None)
    r = x - A @ p
    return float(r @ r), p, r


def coarse_w(x):
    n = len(x)
    win = np.hanning(n)
    spec = np.abs(np.fft.rfft((x - x.mean()) * win, 16 * n))
    k = int(np.argmax(spec[16:])) + 16          # skip DC
    return 2 * math.pi * k / (16 * n)


def refine_w(x, w0, segs, span=None):
    """Golden-section search of the rss over w around w0."""
    span = span or (2 * math.pi / len(x))
    lo, hi = w0 - span, w0 + span
    g = (math.sqrt(5) - 1) / 2
    c, d = hi - g * (hi - lo), lo + g * (hi - lo)
    fc, fd = fit_fixed(x, c, segs)[0], fit_fixed(x, d, segs)[0]
    for _ in range(60):
        if fc < fd:
            hi, d, fd = d, c, fc
            c = hi - g * (hi - lo)
            fc = fit_fixed(x, c, segs)[0]
        else:
            lo, c, fc = c, d, fd
            d = lo + g * (hi - lo)
            fd = fit_fixed(x, d, segs)[0]
    return (lo + hi) / 2


def fit_record(x):
    """Frequency in cycles/sample, seam-robust. Returns dict.

    A record may be a rotation of one continuous segment, so: fit without a
    seam; scan every seam position (two segments, one shared frequency); keep
    the seam only if it cuts the residual by more than 4x."""
    n = len(x)
    span = 3 * 2 * math.pi / n
    w = refine_w(x, coarse_w(x), [(0, n)], span)
    rss0, p, r = fit_fixed(x, w, [(0, n)])
    best = (rss0, None, w)
    ws = w
    for _ in range(2):
        cand = [(fit_fixed(x, ws, [(0, s), (s, n)])[0], s) for s in range(16, n - 16, 4)]
        rs, s = min(cand)
        cand = [(fit_fixed(x, ws, [(0, t), (t, n)])[0], t) for t in range(max(16, s - 4), min(n - 16, s + 5))]
        rs, s = min(cand)
        ws = refine_w(x, ws, [(0, s), (s, n)], span)
        rs = fit_fixed(x, ws, [(0, s), (s, n)])[0]
        if rs < best[0]:
            best = (rs, s, ws)
    seam = None
    if best[1] is not None and best[0] * 4 < rss0:
        seam, w = best[1], best[2]
        rss, p, r = fit_fixed(x, w, [(0, seam), (seam, n)])
    else:
        rss = rss0
    amp = math.hypot(p[0], p[1])
    rms = math.sqrt(rss / n)
    return {"cps": w / (2 * math.pi), "amp_counts": amp, "rms_resid": rms, "seam": seam,
            "snr_db": 20 * math.log10(amp / math.sqrt(2) / max(rms, 1e-9))}


# ── commands ─────────────────────────────────────────────────────────────
def run_control():
    f = gen("sine", 1000, 3.0)
    dev_set("timebase", "0x0E")
    time.sleep(1.0)
    rows = []
    for k, x, m in capture(5, "1,2"):
        x = x[m["skip"]:]
        fr = fit_record(x)
        rows.append({"frame": k[0], "ch": k[1], "mean": float(x.mean()), "pp": float(x.max() - x.min()),
                     **fr})
        print("%s frame %d  mean %.1f  pp %.0f  amp %.1f counts  %.6f cps -> %.1f Hz at 49930 S/s  SNR %.1f dB"
              % (k[1], k[0], x.mean(), x.max() - x.min(), fr["amp_counts"], fr["cps"],
                 fr["cps"] * 49930.1, fr["snr_db"]))
    json.dump({"gen_hz": f, "rows": rows}, open(os.path.join(HERE, "control-%s.json" % time.strftime("%H%M%S")), "w"), indent=1)


# Ladder guess per code (PR #51 / EXP-17/18), used only to pick a tone that
# lands at a sensible number of cycles per record; the fit does not use it.
LADDER = {0x06: 25e6, 0x07: 12.5e6, 0x08: 5e6, 0x09: 2.5e6, 0x0A: 1.25e6, 0x0B: 500e3,
          0x0C: 250e3, 0x0D: 125e3, 0x0E: 50e3, 0x0F: 25e3, 0x10: 12.5e3, 0x11: 5e3,
          0x12: 2.5e3, 0x13: 1.25e3, 0x14: 500.0}


def tones_for(code, n=896):
    fs = LADDER[code]
    out = []
    for cycles in (7.3, 23.7, 61.1, 137.9):          # non-integer: no bin lock
        f = round(cycles * fs / n)
        if 1 <= f <= 50000 and f < 0.4 * fs:
            out.append(f)
    return out or [50000]


def run_rates(codes):
    rows = []
    for code in codes:
        dev_set("timebase", "0x%02X" % code)
        time.sleep(1.0)
        for f in tones_for(code):
            fa = gen("sine", f, 3.0)
            time.sleep(0.8)
            caps = capture(6, "1")
            fits = []
            for k, x, m in caps:
                x = x[m["skip"]:]
                fr = fit_record(x)
                fits.append(fr)
            cps = np.array([q["cps"] for q in fits])
            fs_each = fa / cps
            med = float(np.median(fs_each))
            spread = float((fs_each.max() - fs_each.min()) / med)
            rows.append({"code": code, "req_hz": f, "gen_hz": fa, "fs_each": fs_each.tolist(),
                         "fs_median": med, "spread": spread, "fits": fits})
            print("0x%02X  gen %10.3f Hz  fs %12.1f S/s  (vs ladder %+.3f%%)  spread %.2e  seams %d/%d  SNR %.0f dB"
                  % (code, fa, med, 100 * (med / LADDER[code] - 1), spread,
                     sum(1 for q in fits if q["seam"]), len(fits), np.median([q["snr_db"] for q in fits])))
    json.dump(rows, open(os.path.join(HERE, "rates-%s.json" % time.strftime("%H%M%S")), "w"), indent=1)


def run_gain(ranges):
    """Square at 1 kHz, amplitude steps; counts per GEN code per range."""
    rows = []
    dev_set("timebase", "0x0E")
    for rng in ranges:
        dev_set("ch1.vdiv", str(rng))
        time.sleep(1.0)
        pts = []
        for amp in (0.5, 1.0, 1.5, 2.0, 2.5, 3.0):
            gen("dc", 1, amp)
            time.sleep(0.8)
            hi = np.mean([x[m["skip"]:].mean() for k, x, m in capture(3, "1")])
            gen("dc", 1, 0.0)
            time.sleep(0.8)
            lo = np.mean([x[m["skip"]:].mean() for k, x, m in capture(3, "1")])
            code_hi = 20 + int(round(amp * 10)) * 4095 // 33
            pts.append({"amp": amp, "code_hi": code_hi, "code_lo": 20, "mean_hi": float(hi), "mean_lo": float(lo)})
            print("range %d  amp %.1f  DC code %d -> %.2f counts   code 20 -> %.2f counts"
                  % (rng, amp, code_hi, hi, lo))
        rows.append({"range": rng, "points": pts})
    json.dump(rows, open(os.path.join(HERE, "gain-%s.json" % time.strftime("%H%M%S")), "w"), indent=1)


def run_badge():
    rows = []
    for code in (0x0E, 0x0F, 0x10):
        dev_set("timebase", "0x%02X" % code)
        time.sleep(1.0)
        for f in tones_for(code):
            fa = gen("sine", f, 3.0)
            time.sleep(1.5)
            out = shell("fpga scope freq")
            rows.append({"code": code, "gen_hz": fa, "device": out})
            print("0x%02X gen %.3f Hz  device: %s" % (code, fa, " | ".join(out.strip().splitlines()[:2])))
    json.dump(rows, open(os.path.join(HERE, "badge-%s.json" % time.strftime("%H%M%S")), "w"), indent=1)


if __name__ == "__main__":
    cmd = sys.argv[1]
    args = [int(a, 0) for a in sys.argv[2:]]
    if cmd == "control":
        run_control()
    elif cmd == "rates":
        run_rates(args or sorted(LADDER))
    elif cmd == "gain":
        run_gain(args or [4, 5, 6, 7, 8, 9])
    elif cmd == "badge":
        run_badge()
