# EXP-84: CH2's gain per range, and TMR13/PA6 is its offset reference

- **Date:** 2026-11-26
- **Unit:** my unit
- **Build:** `guest-coldtrace` + EXP-82's fix (`Nov 26 2026 11:01:05`)
- **Setup:** GEN into CH2 by a BNC cable; CH2 x1, DC; CH1 open; timebase 0x0E, AUTO
- **Source scale:** EXP-80, 0.7937 mV per GEN DAC code
- **Status:** CONFIRMED (both)

## 1. Problem

Two open CH2 questions. `scope_cal.c` CH2 rows came from unit #1 (EXP-08);
how do they compare with my unit and an absolute source? And CLAUDE.md lists
CH2's vertical offset reference as "TMR13_CH1 PWM-DAC on PA6, DECODED, NOT
CONFIRMED": does `trig2 raw` really move CH2's level?

## 2. Hypotheses

- Offset: if TMR13/PA6 is CH2's reference, `trig2 raw` moves CH2's level and
  not CH1's, the mirror of what DAC1 (`trig raw`) does to CH1. Falsifier: CH2
  does not move, or CH1 moves with it.
- Gain: CH2 forms the same 1-2-5 ladder as CH1 (EXP-83) within a few percent.

## 3. Procedure

- `offset_ref.py` (in `captures/exp84/`): GEN at 1.0 V DC into CH2; `trig raw`
  2048 / 1024 / 3072 / 2048, then `trig2 raw` 1024 / 2048 / 3072 / 2048; 3
  captures of both channels after each.
- `loop.py gain2`: as EXP-83's gain sweep, on CH2, ranges 4..9, with
  `trig2 raw 2048` left in place (CH2's level then sits near 41 counts, so
  0-3 V stays on screen).

## 4. Control

| control | expected | measured | passed? |
|---|---|---|---|
| GEN reaches CH2 | the 1 kHz sine | 0.020000 cycles/sample, 66 counts | yes (clipping at the top: CH2 sat near 101 counts before TMR13 was set) |
| DAC1 moves CH1 (`trig raw`) | CH1 moves, CH2 does not | CH1 0 / 48.9 / 173.3 at 1024 / 2048 / 3072; CH2 152.1-152.2 throughout | yes |

## 5. Results

### Offset reference

| `trig2 raw` | CH1 | CH2 |
|---:|---:|---:|
| 1024 | 48.89 | 0.00 (railed low) |
| 2048 | 48.89 | 90.81 |
| 3072 | 48.97 | 215.23 |
| 2048 | 48.86 | 91.09 |

TMR13 moves CH2 and leaves CH1 alone, about 124 counts per 1024 codes, the
same sensitivity DAC1 has on CH1. Before the sweep CH2 sat ~50 counts higher
than at `trig2 raw 2048` (152 with GEN at 1 V, so its zero near 101): this
image does not set TMR13 to the mid-scale that DAC1 gets.

### Gain (CH2)

| range | CH2 mV/count | slope SE | CH1 (EXP-83) | CH1/CH2 difference | unit #1 table x 0.92 |
|---|---:|---:|---:|---:|---:|
| 4 | 7.82 (2 points) | | 7.85 | 0.4 % | 8.73 |
| 5 | 19.86 | 0.46 % | 20.01 | 0.8 % | 19.28 |
| 6 | 39.39 | 1.04 % | 39.79 | 1.0 % | 38.37 |
| 7 | 77.11 | 1.15 % | 78.90 | 2.3 % | 77.09 |
| 8 | 206.53 | 1.87 % | 194.87 | 5.8 % | 205.81 |
| 9 | 430.48 | 2.53 % | 404.21 | 6.3 % | 391.00 |

Zero (GEN at code 20) 41.9 to 47.1 counts across ranges.

## 6. Blind spots

- Ranges 8/9 rest on 7-15 counts of span (GEN stops at 3 V), so their 6 %
  channel difference is within about two standard errors and is not resolved.
- Range 4 has two levels below the rail.
- The offset sweep used three codes; the reference's linearity and the right
  per-range centre value are not measured here.

## 7. Conclusion

- **Established:** TMR13/PA6 is CH2's vertical offset reference (moves CH2
  only, same sensitivity as DAC1 on CH1). CH2's gain ladder matches CH1's
  within 0.4-2.3 % on ranges 4-7.
- **Follow-up:** set TMR13 at boot the way DAC1 is set (CH2 otherwise sits
  ~50 counts off and clips a 0-3 V signal); per-range centring for both
  channels (the zero sits at 42-50 counts, not mid-scale) belongs with the
  display work.
