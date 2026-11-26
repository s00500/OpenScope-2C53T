#!/usr/bin/env python3.11
"""EXP-89: shorted zero against the GEN sweep's intercept, per range.

  python3.11 analyse.py ch1 zero-ch1-203738.json gensweep-ch1-210522.json
  python3.11 analyse.py ch2 zero-ch2-204607.json gensweep-ch2-204100.json gensweep-ch2-204251.json

The zero is the 2048 reading from the shorted run; later sweep files win for
a range they repeat. GEN offset = (intercept - zero) x gain."""
import json, os, sys, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
ch, zf, gfs = sys.argv[1], sys.argv[2], sys.argv[3:]
zr = {(r['range'], r['code']): r for r in json.load(open(os.path.join(HERE, zf)))['rows']}
fits = {}
for f in gfs:
    for row in json.load(open(os.path.join(HERE, f)))['rows']:
        if row['fit'] and row['fit']['n'] >= 3:
            fits[row['range']] = row['fit']
bs = []
for r in sorted(fits):
    f, z0 = fits[r], zr[(r, 2048)]
    se_i = f['se']['intercept']
    d = f['intercept'] - z0['mean']
    b, se_b = d * f['mv_per_count'], np.hypot(se_i, z0['frame_sd'] / 2) * f['mv_per_count']
    bs.append((b, se_b))
    print("%s r%d  n=%d  gain %.3f mV/ct (SE %.2f%%)  resid %.2f  intercept %.2f  shorted %.2f  diff %+.2f ct  GEN offset %+.1f +- %.1f mV"
          % (ch, r, f['n'], f['mv_per_count'], 100 * f['se']['slope_rel'], f['max_resid'], f['intercept'], z0['mean'], d, b, se_b))
w = np.array([1 / s**2 for _, s in bs]); v = np.array([b for b, _ in bs])
m = float((w * v).sum() / w.sum())
print("weighted GEN offset %.1f +- %.1f mV, chi2 %.1f on %d dof" % (m, w.sum()**-0.5, float((w * (v - m)**2).sum()), len(bs) - 1))
