#!/usr/bin/env python3.11
"""Autoset on my unit: GEN into CH2 (CH1 open), each case from deliberately
wrong settings, AUTO pressed, then the settings read back once they stop
changing. Expectations are fixed here before the run (see the table in the
commit / devlog); a screenshot of each result is saved next to this file.

Restores the settings it found and saves them.
"""
import json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
OS = os.environ.get("OPENSCOPE", "openscope")
KEYS = ["ch1.vdiv", "ch2.vdiv", "timebase", "trig.source", "trig.level", "trig.mode",
        "trig.code", "ch1.enabled", "ch2.enabled"]


def sh(*a, timeout=60):
    r = subprocess.run([OS, *a], capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError("%s: %s %s" % (" ".join(a), r.stdout[-300:], r.stderr[-300:]))
    return r.stdout


def get():
    return json.loads(sh("--json", "get"))


def snap():
    d = get()
    return {k: d[k] for k in KEYS}


def status():
    for line in sh("shell", "fpga scope autoset").splitlines():
        if line.startswith("phase"):
            return line.strip()
    return ""


def run_autoset(timeout=25.0):
    """Press AUTO once nothing is running, then wait for the autoset's own
    state to read `phase off` (a quiet spell of settings is not the end: it
    waits for captures between steps without changing anything)."""
    t = time.time()
    while not status().startswith("phase off") and time.time() - t < timeout:
        time.sleep(0.3)
    sh("press", "auto")
    t0 = time.time()
    time.sleep(0.3)
    while time.time() - t0 < timeout:
        st = status()
        if st.startswith("phase off"):
            return snap(), time.time() - t0, st
        time.sleep(0.3)
    return snap(), None, status()


CASES = [
    # name, siggen command, starting settings, expected
    ("sine-1k", "siggen wave 0 amp 30 duty 50 freq 1000 on",
     {"ch2.vdiv": "9", "timebase": "0x08"}, {"ch2.vdiv": 6, "timebase": 0x0D, "ch1.vdiv": 5, "trig.source": "ch2"}),
    ("square-20k", "siggen wave 1 amp 30 duty 50 freq 20000 on",
     {"ch2.vdiv": "2", "timebase": "0x14"}, {"ch2.vdiv": 6, "timebase": 0x09, "ch1.vdiv": 5, "trig.source": "ch2"}),
    ("dc-1v5", "siggen wave 9 amp 15 duty 50 freq 1 on",
     {"ch2.vdiv": "9", "timebase": "0x0E"}, {"ch2.vdiv": 5, "timebase": 0x0E, "ch1.vdiv": 5, "trig.source": "ch2"}),
    ("off", "siggen off",
     {"ch2.vdiv": "9", "timebase": "0x0B"}, {"ch2.vdiv": 5, "timebase": 0x0B, "ch1.vdiv": 5}),
    ("sine-1k-ch2-alone", "siggen wave 0 amp 30 duty 50 freq 1000 on",
     {"ch1.enabled": "off", "ch2.vdiv": "9", "timebase": "0x08"}, {"ch2.vdiv": 5, "timebase": 0x0D, "trig.source": "ch2"}),
]

before = get()
results = []
try:
    for name, gen, start, want in CASES:
        sh("shell", gen)
        for k, v in start.items():
            sh("set", "--", k, v)
        time.sleep(1.5)
        got, took, st = run_autoset()
        ok = all(got.get(k) == v for k, v in want.items())
        results.append({"case": name, "start": start, "want": want, "got": got,
                        "seconds": took, "status": st, "pass": ok})
        print("%-18s %s  %.1f s  %s" % (name, "PASS" if ok else "FAIL", took or -1,
              {k: got[k] for k in ("ch1.vdiv", "ch2.vdiv", "timebase", "trig.source", "trig.level", "trig.code")}))
        print("    " + st[:200])
        if not ok:
            print("    want", want)
        sh("screenshot", os.path.join(HERE, "device-%s.png" % name), timeout=120)
        if start.get("ch1.enabled") == "off":
            sh("set", "ch1.enabled", "on")
finally:
    sh("shell", "siggen wave 0 amp 30 duty 50 freq 1000 off")
    for k in ("ch1.enabled", "ch2.enabled", "ch1.vdiv", "ch2.vdiv", "trig.source", "trig.mode"):
        sh("set", "--", k, str(before[k]).lower() if isinstance(before[k], bool) else str(before[k]))
    sh("set", "--", "timebase", "0x%02X" % int(before["timebase"]))
    sh("set", "--", "trig.level", str(before["trig.level"]))
    sh("action", "save")
    after = get()
    diff = {k: (before[k], after.get(k)) for k in before if before[k] != after.get(k) and k != "capture.gen"}
    print("restored; settings diff:", diff)
    json.dump(results, open(os.path.join(HERE, "autoset-%s.json" % time.strftime("%H%M%S")), "w"), indent=1)
print("%d/%d passed" % (sum(r["pass"] for r in results), len(results)))
