import csv, math, sys, statistics, collections
def load(p):
    recs = collections.defaultdict(list); meta = {}
    for r in csv.DictReader(open(p)):
        k = (r["frame_id"], r["channel"]); recs[k].append(int(r["count"])); meta[k] = (float(r["sample_rate_hz"]), int(r["head_skip"]))
    return recs, meta
def tone(x, fs, f):
    n = len(x); m = sum(x) / n
    w = [0.5 - 0.5 * math.cos(2 * math.pi * i / (n - 1)) for i in range(n)]
    re = sum((x[i] - m) * w[i] * math.cos(2 * math.pi * f * i / fs) for i in range(n))
    im = sum((x[i] - m) * w[i] * math.sin(2 * math.pi * f * i / fs) for i in range(n))
    return 2 * math.hypot(re, im) / sum(w)   # amplitude in counts (peak)
for p in sys.argv[1:]:
    recs, meta = load(p)
    for ch in ("CH1", "CH2"):
        a1, a3, sd, mean = [], [], [], []
        for k, x in recs.items():
            if k[1] != ch: continue
            fs, skip = meta[k]; x = x[skip:]
            a1.append(tone(x, fs, 1000.0)); a3.append(tone(x, fs, 3000.0)); sd.append(statistics.pstdev(x)); mean.append(statistics.mean(x))
        print("%-12s %s  n=%2d  mean %.1f  std %.2f  1kHz %.3f  3kHz %.3f counts (peak)" % (p, ch, len(a1), statistics.mean(mean), statistics.mean(sd), statistics.mean(a1), statistics.mean(a3)))
