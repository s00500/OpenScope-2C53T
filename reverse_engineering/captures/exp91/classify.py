#!/usr/bin/env python3.11
"""EXP-91: run the firmware's own edge classifier (src/dsp/trig_edge.c, built
as a shared library) over captured records, and show where it looks.

  python3.11 classify.py <lib.so> <capture.csv> <crossing> [channel]

crossing = reg 0x08 code - 28 (record units). Per record: the seam the value
step finds (k), the classification, and the record around index 512 after
rotating by k (the classifier's own view)."""
import csv, ctypes, sys
lib = ctypes.CDLL(sys.argv[1])
path, crossing = sys.argv[2], int(sys.argv[3])
want = sys.argv[4] if len(sys.argv) > 4 else "CH2"
lib.trig_edge_classify.restype = ctypes.c_int
lib.trig_edge_find_seam.restype = ctypes.c_int
names = {0: "UNKNOWN", 1: "RISING", 2: "FALLING"}
recs, meta = {}, {}
for r in csv.DictReader(open(path)):
    k = (int(r["frame_id"]), r["channel"])
    recs.setdefault(k, []).append(int(r["count"]))
    meta[k] = r["time_ordered"]
for (fid, ch), v in sorted(recs.items()):
    if ch != want or len(v) != 1024:
        continue
    buf = (ctypes.c_uint8 * 1024)(*v)
    k = ctypes.c_uint32(0)
    seam = lib.trig_edge_find_seam(buf, ctypes.byref(k))
    cls = names[lib.trig_edge_classify(buf, crossing)]
    r = v[k.value:] + v[:k.value] if seam else v
    lo, hi = min(v[128:]), max(v[128:])
    near = r[500:525:3]
    print("frame %d %s ordered=%s seam=%s k=%d class=%s span %d..%d  r[500..524:3]=%s"
          % (fid, ch, meta[(fid, ch)], bool(seam), k.value, cls, lo, hi, near))
