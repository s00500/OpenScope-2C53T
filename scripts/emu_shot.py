#!/usr/bin/env python3
"""Drive the Renode emulator build: press keys, save exact LCD frames, read
the scope's redraw counters (EXP-85).

    cd firmware && make emu
    python3.11 scripts/emu_shot.py --out .scratchpad/emu \
        "wait:8 key:menu key:menu key:menu wait:4 shot:scope count:idle \
         key:trigger wait:4 shot:cursor-v count:cursor-v"

Steps (space separated):
  key:<name>    press a button through the g_emu_key mailbox
                (ch1 ch2 move select trigger prm auto save menu up down left
                 right ok)
  wait:<s>      let the machine run for s host seconds
  shot:<name>   pause, flush the LCD model (NOP), save <out>/<name>.png, resume
  count:<label> print ui_scope_full_draws / ui_scope_live_draws / skipped
                since the previous count step

Needs Renode at /Applications/Renode.app and Pillow (python3.11 has it).
The emulator build renders the scope from a synthetic capture
(fpga_emu_feed), through the same live compositor as the instrument; a
device screenshot cannot show a live frame, because it pauses RUN first.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RENODE = "/Applications/Renode.app/Contents/MacOS/renode"
RESC_DIR = os.path.join(ROOT, "emulator", "renode")
ELF = os.path.join(ROOT, "firmware", "build", "firmware.elf")
FB = "/tmp/openscope_fb.bin"
LCD_CMD = 0x6001FFFE

BUTTONS = ["ch1", "ch2", "move", "select", "trigger", "prm", "auto", "save",
           "menu", "up", "down", "left", "right", "ok"]   # ids 1..14


def symbols(names):
    out = subprocess.run(["arm-none-eabi-nm", ELF], capture_output=True,
                         text=True, check=True).stdout
    found = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] in names:
            found[parts[2]] = int(parts[0], 16)
    missing = [n for n in names if n not in found]
    if missing:
        sys.exit("symbols not in %s: %s (is this an emu build?)" % (ELF, missing))
    return found


def fb_to_png(src, dst):
    from PIL import Image
    d = open(src, "rb").read()
    w, h = 320, 240
    im = Image.new("RGB", (w, h))
    px = im.load()
    for i in range(w * h):
        c = struct.unpack_from("<H", d, i * 2)[0]
        px[i % w, i // w] = (((c >> 11) & 31) * 255 // 31,
                             ((c >> 5) & 63) * 255 // 63,
                             (c & 31) * 255 // 31)
    im.save(dst)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("steps", help="space separated steps, see above")
    ap.add_argument("--out", default=os.path.join(ROOT, ".scratchpad", "emu"))
    ap.add_argument("--timeout", type=float, default=600.0)
    args = ap.parse_args()

    args.out = os.path.abspath(args.out)
    os.makedirs(args.out, exist_ok=True)
    sym = symbols(["g_emu_key", "ui_scope_full_draws", "ui_scope_live_draws",
                   "ui_scope_skipped"])

    lines = ["include @run_openscope.resc"]
    shots = []
    for step in args.steps.split():
        kind, _, val = step.partition(":")
        if kind == "key":
            if val not in BUTTONS:
                sys.exit("unknown key %r" % val)
            lines.append("sysbus WriteByte 0x%08X %d" % (sym["g_emu_key"],
                                                         BUTTONS.index(val) + 2))
            lines.append("sleep 1")
        elif kind == "wait":
            lines.append("sleep %s" % float(val))
        elif kind == "shot":
            raw = os.path.join(args.out, val + ".bin")
            lines += ["pause",
                      "sysbus WriteWord 0x%08X 0x0000" % LCD_CMD,
                      "python \"__import__('shutil').copyfile('%s', '%s')\"" % (FB, raw),
                      "start"]
            shots.append((raw, os.path.join(args.out, val + ".png")))
        elif kind == "count":
            lines.append("echo \"COUNT %s\"" % val)
            for n in ("ui_scope_full_draws", "ui_scope_live_draws", "ui_scope_skipped"):
                lines.append("sysbus ReadDoubleWord 0x%08X" % sym[n])
        else:
            sys.exit("unknown step %r" % step)
    lines += ["pause", "quit"]

    with tempfile.NamedTemporaryFile("w", suffix=".resc", dir=RESC_DIR,
                                     delete=False) as f:
        f.write("\n".join(lines) + "\n")
        resc = f.name
    try:
        res = subprocess.run([RENODE, "--disable-gui", "--console", resc],
                             cwd=RESC_DIR, capture_output=True, text=True,
                             timeout=args.timeout)
    finally:
        os.unlink(resc)

    text = re.sub(r"\x1b\[[0-9;]*m", "", res.stdout + res.stderr)
    errors = [l for l in text.splitlines() if "error" in l.lower()]
    for l in errors[:10]:
        print("renode:", l)

    # COUNT label, then three hex values.
    prev = None
    vals = re.findall(r"COUNT (\S+)|^(0x[0-9A-Fa-f]+)\s*$", text, re.M)
    label, nums = None, []
    rows = []
    for lab, num in vals:
        if lab:
            label, nums = lab, []
        elif label is not None:
            nums.append(int(num, 16))
            if len(nums) == 3:
                rows.append((label, nums))
                label = None
    for label, (full, live, skip) in rows:
        if prev is None:
            print("%-14s full=%5d live=%5d skipped=%5d (totals)" % (label, full, live, skip))
        else:
            print("%-14s full=%+5d live=%+5d skipped=%+5d" %
                  (label, full - prev[0], live - prev[1], skip - prev[2]))
        prev = (full, live, skip)

    for raw, png in shots:
        if os.path.exists(raw):
            fb_to_png(raw, png)
            os.unlink(raw)
            print("saved", png)
        else:
            print("MISSING", raw)


if __name__ == "__main__":
    main()
