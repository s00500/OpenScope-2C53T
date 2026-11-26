# EXP-88: Scale presses without the blink; trigger point and channel references on the glass

- **Date:** 2026-11-26
- **Unit:** my unit
- **Build:** `guest-coldtrace`, this change (`Nov 26 2026 17:24:15`)
- **Setup:** generator off, both channels on (range 5), AUTO, timebase 0x08,
  cursors off, no mask; the trigger level was -50 (set on the unit's keys
  between builds, left as found)
- **Status:** CONFIRMED

## 1. Problem

After EXP-87 every arrow press drew through the live compositor except
MOVE's Scale mode: volts/div and time/div sent `DCMD_REDRAW_ALL`, which
clears the whole screen. The user asked for that blink to go too. They also
asked whether the trigger point is shown (barely: a 7 x 4 triangle on the
band's first rows, under the top-row labels, and only when a trigger locked)
and for a reference indicator per channel (the old ground markers were drawn
by the full repaint only, at mid-screen whatever the channel's band, and the
first live frame erased them).

## 2. Hypothesis

A Scale change needs only three things redrawn: the trace (compositor), the
info bar (it already has its own epoch) and the measurement badges (full
path only). With volts/div and time/div moved to the live epoch, the bar and
the badges written in place (opaque text boxes, no clear), and the badges
refreshed once from the first record committed at the new setting, Scale
presses make 0 full repaints. A run/stop toggle still makes one each (the
counter works). Falsifier: full repaints on Scale presses, or none on the
control.

## 3. Procedure

Change:

- `main.c`: `scope_scale_epoch()` (timebase, CH1/CH2 range) left
  `scope_ui_epoch()` for `scope_live_epoch()`. The display loop refreshes the
  badges (`scope_ui_badges_refresh()`) once per scale change, when the frame
  generation moves past the one in force at the change, or after 1 s, or at
  once when stopped. Once per change is what the full repaint it replaces
  cost per press, not per live frame (which once stalled acquisition, see
  `draw_scope_live_frame`).
- `status_bar.c`: the scope time-view info bar is written field by field
  (`font_draw_string_box` per field, the margin rows filled), not cleared.
- `scope_ui.c`: `draw_one_badge` and the row-2 slots write each pixel once.
- Trigger point (`trig_point_get`): the triangle moved under the top row of
  labels, drawn at the requested column in grey when nothing locked, and a
  dotted line down the band in Trigger and Position modes; both renderers.
- Channel references: a "1" / "2" chip on the left edge at
  `scope_vmap_ref_y()` (autofit: the middle of the fitted window; true scale:
  code 128), moving with the position, with a pixel tip; both renderers
  (`test-scope-vmap`: the reference row tracks the trace centre, code 128 and
  the offset). `draw_ground_markers` removed. `CHIP_MAX` 16 -> 17, with a
  static assert that chips plus the popup fit `BAND_MAX_HOLES`.
- Scale keys send `DCMD_SCOPE_LIVE` with the popup.

Instrument: the `display` shell command's counters, as in EXP-87, no
screenshots during a counted sequence.

**Preconditions verified by readback:**

| what | expected | measured |
|---|---|---|
| build running | 17:24:15 | `shell version` |
| MOVE mode | Scale | shell `mask`: `move-mode Scale` |
| state after the presses | range 5, timebase 0x08, running | `get`: unchanged |

## 4. Control

| control | expected | measured | passed? |
|---|---|---|---|
| OK, OK (stop, run) | full +2 (redraw_all) | full 1 -> 3, redraw_all 0 -> 2 | yes |

## 5. Results

| presses (Scale mode) | full repaints | live frames |
|---|---|---|
| UP, DOWN (CH1 range 5 -> 6 -> 5), RIGHT, LEFT (timebase and back) | +0 | +74 |

Live frame rate, no presses: 84 and 85 per 10 s (EXP-85 left it at 83-84);
last frame 84.4 ms. The extra per-pixel tests cost nothing measurable.

Emulator (`scripts/emu_shot.py`): V/div up, time/div right, both back,
Trigger and Position presses: 0 full repaints.

Screenshots (`captures/move-modes/device2-*.png`, `emu2-*.png`; weak
evidence): the trigger point is visible under the labels (grey on my unit, no
lock with the generator off; trigger colour in the emulator, locked), the
dotted line in Trigger and Position, the "1" / "2" flags on their traces,
and the "1" moving up with a Position press.

## 6. Blind spots

- The badge refresh was not timed against a record arriving at a new range
  with a signal on: with the generator off, the badges read the same before
  and after. That the refresh waits for a new record is from the code, not
  a measurement.
- The counter cannot see a sub-frame flash a human would; the bar and badge
  rows were checked by construction (each pixel written once), not by camera.
- The reference flag is not a 0 V marker in autofit, and this test does not
  try to make it one.

## 7. Conclusion

- **Established:** volts/div and time/div presses no longer repaint the
  screen on my unit (0 full repaints, control 2); the frame rate is
  unchanged; the trigger point and the channel references are drawn by both
  renderers.
- **Not established:** that the refreshed badges always show the new
  record (see blind spots).
