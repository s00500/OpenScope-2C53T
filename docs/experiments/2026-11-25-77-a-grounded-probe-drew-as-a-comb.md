# EXP-77: a grounded probe drew as a comb

- **Date:** 2026-11-25
- **Unit:** bench unit #1
- **Build:** `make guest-coldtrace` from `feat/remote-control` (uncommitted
  working tree), flashed over CDC (`cdc_flash.py`, slot b) as
  `exp77-vmap.bin`, sha256 `aa28f9b0…`, build "Nov 25 2026 21:36:03".
  Baseline image: EXP-76's `88530f38…`.
- **Status:** CONFIRMED (rendering defects fixed; the zero offset that
  remains is a separate, known gap)

## 1. Problem

With the CH1 probe clipped to its own ground (probe x10, DC, range 6), the
user saw "quite a mess" on the scope screen. Is the instrument wrong, or the
drawing?

## 2. Hypothesis

The data is clean and the renderer magnifies and misplaces it. If true,
records span about one ADC count of noise while the screen shows a trace many
rows tall, and trace pixels appear inside the readout rows. If false, the
records themselves are spread or jumping.

## 3. Procedure

- Records: `openscope capture --frames 3` (GET_WAVEFORM).
- Screen: `openscope screenshot --no-hold --torn --scale 1` (the LCD shadow,
  palette indices), measured by `lcdmetric.py`: rows 30..192 (trace area) with
  at least 3 pixels of CH1 yellow (palette 5) or CH2 cyan (6), and the count of
  each colour in the readout strip, rows 193..223.
- Settings, read back: scope mode, 0x0E (641 us/div), CH1 r6 x10 DC
  (12.64 V/div), CH2 r4 x10 AC, both channels on, AUTO, graticule autofit.

## 4. Control

| control | expected | measured | passed? |
|---|---|---|---|
| records with the probe grounded (the data) | ~1 count of noise | CH1 45..48 counts, mean 46.0; CH2 103..106 | yes |
| baseline screen, old image, 3 shots | (the defect, to compare against) | CH1 trace 26 / 38 / 26 rows tall at rows 81..119; strip cyan 422 | recorded |

## 5. Results

**Causes, from the code (scope_ui.c):**

1. **Zoom.** Autofit stretched the record's min..max over its band with a
   floor of only 8 counts. With both channels on the CH1 band is ~100 rows,
   so one count drew ~12 rows: +/-1 count of ADC noise became a comb.
2. **Pinned.** The record's minimum was drawn at the band's bottom edge,
   so a quiet trace sat on the edge, and CH2's on the readout rows.
3. **Stale strip.** The full repaint drew traces down to SCOPE_BOT, through
   the readout rows, then painted the readouts on top; the live compositor
   repaints only down to BADGE_ROW2_Y. Trace pixels in the strip were never
   cleared until the next full repaint (the cyan dashes in the readouts).
4. **Second transform.** The compositor always autofitted, even with the
   true-scale graticule on, while the full repaint honoured it: the trace
   jumped scale whenever the renderer changed (from the code; not measured on
   the old image).

**Fix:** `src/ui/scope_vmap.{c,h}`, one ADC-count-to-row map used by the full
repaint, the compositor and the trigger marker (`chan_map_prep()` in
scope_ui.c). Autofit is capped at 4x the calibrated rows per count and a
record quieter than the cap is centred; true scale keeps 26 rows per 32
counts; every band ends at TRACE_BOT (= BADGE_ROW2_Y), above the readouts.
Host test `make test-scope-vmap`: zoom cap, centring, clamping, true-scale
geometry, cursor scale; the old 8-count floor (a 3-count record spans
31 rows) and a pinned-to-bottom variant both fail it.

**On the device, after (same settings, 3 shots):**

| | before | after |
|---|---|---|
| CH1 trace height | 26 / 38 / 26 rows | 9 / 9 / 12 rows |
| CH1 trace position | rows 81..119, on the band's bottom edge (120) | rows 56..67, band middle ~60 |
| CH2 trace | in the readout strip | rows 143..154, band middle ~149 |
| readout strip cyan | 422 px | 96 px (the "CH2pp" text only) |
| readout strip yellow | 203 px | 206 px (the readout text) |

**True scale, live (compositor) vs held (full repaint), same frame
settings:** CH1 on row 103 in both, CH2 trace rows 166..169 in both. The
held frame's extra cyan at rows 117..124, x 0..10 is the "<2" channel marker,
drawn by the full repaint only.

## 6. Blind spots

- Screenshots read the LCD shadow, not the panel; the panel was not watched
  for this run. The live shots are torn by design (`--torn`), which can only
  add rows to the extents above, never remove them.
- No signal larger than noise was on either input, so "large signals still
  fill the band" rests on the host test only.
- The true-scale "jump" on the old image was not measured, only read from the
  code.

## 7. Conclusion

- **Established:** the records were clean (+/-1 count); the mess was the
  renderer. Fixed and measured: trace height down ~3x, quiet traces centred,
  nothing drawn into the readout strip, one transform for every renderer,
  true scale consistent between them.
- **Not fixed (separate):** the zero offset. A grounded CH1 reads 46 counts,
  not mid-scale 128, about 2.6 divisions low at every range (EXP-76 range
  sweep: 43.6..49.3). Autofit hides it; in true scale the grounded trace clamps
  to its band's edge. `fpga scope center` (DAC1 servo to code 128) is the
  existing tool for it.
- **Follow-up:** zero per range with the input grounded (`fpga scope center`)
  and whether that should persist; then true scale as the default view.
