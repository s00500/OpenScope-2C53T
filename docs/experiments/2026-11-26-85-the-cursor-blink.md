# EXP-85: the cursor blink, and the labels the live renderer erased

- **Date:** 2026-11-26
- **Unit:** my unit
- **Builds:** before = `guest-coldtrace` at ef70eba (`Nov 26 2026 11:55:11`,
  `.scratchpad/tables.bin`); after = `guest-coldtrace` with this change
  (`Nov 26 2026 15:30:16`); intermediate images A to E below
- **Setup:** scope, timebase 0x08, AUTO, CH1 open (AC, range 5), CH2 on the
  generator cable, generator off; RUN on
- **Status:** CONFIRMED (the blink is gone by the device's own counters; the
  look on the glass is a screenshot plus an emulator frame, see blind spots)

## 1. Problem

Handoff G2: with any cursor on (TRIGGER in the time view), the scope screen
blinks. `scope_incremental_available()` returned false whenever a cursor was
on, so every frame took `draw_scope_screen()`: clear the band, then repaint.
Second symptom, seen on my unit: the cursor readout (dt, 1/dt, dV) was drawn at
`SCOPE_BOT - 28`, on the badge rows, and the badges painted over it, so the
readout never showed. The V1 label sat under the `SPI3:OK` text.

## 2. Hypotheses

- H1: the blink is the clear-and-repaint path, taken because the live
  compositor does not draw cursors. If the compositor draws them, the device's
  full-repaint counter stays at 0 with cursors on. Falsifier: full repaints
  continue with cursors on.
- H2: cursor key presses also blink, because every cursor action sent
  `DCMD_DRAW_SCOPE` (a full repaint) and LEFT/RIGHT auto-repeat at 6.7/s.
  Falsifier: presses do not move the full-repaint counter before the fix.
- H3 (found while reading the code): every label in the trace band (CH label,
  SPI3 counter, trigger status, STOP) is erased by the first live frame after
  a full repaint, because the compositor's only hole was the popup.

## 3. Procedure

Instrument: `ui_scope_full_draws`, `ui_scope_live_draws`, `ui_scope_skipped`
(main.c, already there), read with `mem read` over the shell. Added in this
change: `ui_scope_full_why[4]` (which path asked for each full repaint) and
`ui_scope_live_cyc[_max]` (DWT cycles of the last live frame), both printed by
the shell's `display` command.

- `captures/exp85/rate.sh`: full/live/skipped over a 10 s window.
- `captures/exp85/session2.sh`: per cursor mode, a screenshot to CONFIRM the
  mode, a 10 s window, then 11 cursor presses with `full_why` before/after.
- `captures/exp85/cyc.sh`: the live frame's cost, sampled 10-15 times.
- Emulator: `scripts/emu_shot.py` on `make emu`, which now renders the scope
  through the live compositor from a synthetic capture (`fpga_emu_feed`).

The change itself (scope_ui.c, main.c, redraw_gate.h, input_handler.c,
band_holes.h):

- Cursor lines and the trigger arrow are pixel tests (`cursor_px`,
  `trig_arrow_px`) shared by both renderers; the compositor composites them.
- All text on the band is a chip: an opaque `font_draw_string_box` written
  once per pixel. The compositor cuts chips out of its column stream as holes
  (`band_col_segments`) and paints them after it. The readout moved to the top
  left under the top row; V labels to the bottom of the band, H labels to the
  right edge.
- Cursor state left the main redraw epoch for a "live epoch"; the gate serves
  a live-epoch change through the compositor when it can (`redraw_gate.h` arm
  2b). Cursor keys send the new `DCMD_SCOPE_LIVE` instead of `DCMD_DRAW_SCOPE`.
- The scope popup is sized to its text (it was a fixed 200 px box; G4).
- Cursor keys only act in the time view (the audit's finding: in FFT/XY they
  moved invisible cursors). H cursors clamp to the trace band (row 192).

## 4. Control

| control | expected | measured | passed? |
|---|---|---|---|
| counters move on the old image, cursors off | live > 0, full 0 | live 86-87, full 0 per 10 s | yes |
| the probe reads non-zero (after the awk quoting bug, below) | live > 0 | yes; `rate.sh` now refuses an empty read | yes |
| cursor mode confirmed by screenshot before each window | the mode under test | yes (session2) | yes |

## 5. Results

**Before (old image):**

| state | full / 10 s | live / 10 s |
|---|---|---|
| cursors off | 0 | 86, 87 |
| cursors vertical | **23** | **0** |

**After (final image):**

| state | full / 10 s | live / 10 s |
|---|---|---|
| cursors vertical | 0 | 81 |
| cursors horizontal (image flicker3) | 0 | 74 |
| cursors both | 0 | 84 |
| 18 cursor presses, vertical (mode confirmed; timebase and V/div unchanged after) | **0** | |
| 11 presses in each of V, H, both (image flicker3) | **0**, `full_why` unchanged | |

H1 and H2 confirmed. H3 confirmed in the emulator: the top-row labels now
survive live frames (`emu-live-cursors-*.png`), where before they were erased.

**Frame rate.** Same state, cursors off, three 10 s windows each, images
flashed back to back:

| image | live / 10 s | live frame (wall) |
|---|---|---|
| old (ef70eba) | 85, 86, 86 | not instrumented |
| A: chips painted every frame | 74-76 (other sessions) | 95 ms |
| B: cursor/arrow tests hoisted to per column | 80, 80, 81 | 90.5 ms |
| C: top-row chips flush with the band top (one segment per column) | 82, 82, 82 | 85.4 ms |
| D: shadow position stepped, 2-entry colour cache | 83, 83, 84 | 84.7 ms |
| E/final: 16-entry colour cache | 83, 84, 84 | 83.9 ms |

The final image is ~2 % below the old one with cursors off. That is the cost of
actually keeping the top-row labels on screen (the SPI3 counter changes every
frame) where the old compositor erased them.

**A live frame costs ~84 ms of wall time**, not the "6-8 ms" in
`draw_scope_live_frame`'s header comment. The display task has the lowest
priority, so this includes preemption by the acquisition task; the CPU cost of
the compositor alone is not measured. Skipping the LCD shadow's palette lookup
(by moving `lcd_shadow_page` out of range with `mem write`) took a frame from
103.6 to 87.3 ms, but the two shadow optimizations (D, E) recovered only
~1.5 ms of that; the rest of that 16 ms is unexplained.

**Measurement errors on the way (recorded, both caught):**

1. `rate.sh` once printed all zeros: the address in the awk pattern sat in
   single quotes and never expanded. It now matches with `awk -v` and fails on
   an empty read.
2. Twice I inferred the cursor mode by counting TRIGGER presses and was wrong
   (the mode survives some reboots and not others). The "both mode" press
   bursts then ran with cursors OFF, where UP/DOWN change V/div and LEFT/RIGHT
   the timebase: 14 full repaints per 23 presses, all `DCMD_REDRAW_ALL` by the
   new `full_why` counter, i.e. the V/div popup, not the cursor code. The
   settings were restored (timebase 0x08, CH1 range 5). From then on every
   mode was confirmed by a screenshot first.

## 6. Blind spots

- The counters say what the firmware drew, not what the panel showed. A device
  screenshot pauses RUN and so always shows the full path; the live frames were
  seen only in the emulator. **A look at the glass with cursors on is still
  owed.**
- The emulator's capture is synthetic and its LCD model is slow; it shows
  pixels and draw decisions, not timing.
- Stopped scope: cursor moves now render through the compositor from the FPGA
  buffers. If acquisition keeps writing those while stopped, a cursor move
  would show a newer record; the full path reads the same buffers, so this is
  no change, but it was not checked.
- Still full-path only, so still erased by live frames (unchanged): ground
  markers and the math overlay.
- In `SCOPE_DEBUG_OVERLAY` builds the band ends at row 149, so an H cursor
  below that is not drawn (the clamp is 192).

## 7. Conclusion

- **Established:** the cursor blink was the clear-and-repaint path; with the
  compositor drawing cursors and chips, cursors cost 0 full repaints per 10 s
  (was 23) and cursor presses 0 (were one each). The readout is visible. The
  band's labels survive live frames.
- **Excluded:** the panel self-heal (0 recoveries during the presses).
- **NOT excluded:** a visible artefact on the glass that the counters cannot
  see; the unexplained part of the shadow's cost.
- **Follow-up:** the user decides what TRIGGER should do (handoff G2) and
  where cursors live; the ~84 ms live frame (preemption vs compositor CPU) is
  the next lever for the ~8 frames/s display rate.
