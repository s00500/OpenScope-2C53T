# EXP-83: loopback on CH1: gain per range, crosstalk, and the frequency badge

- **Date:** 2026-11-26
- **Unit:** my unit
- **Build:** crosstalk and badge: `guest-coldtrace` 8c3caaa; gain: + EXP-82's
  fix (`Nov 26 11:01:05`), which the railed points needed
- **Setup:** GEN into CH1 by a BNC cable; CH1 x1, DC; CH2 open, range 5, AC
- **Source scale:** EXP-80, 0.7937 mV per GEN DAC code (absolute ±1 to 2 %)
- **Status:** CONFIRMED (gain, crosstalk); badge characterised

## 1. Problem

`scope_cal.c` holds one gain per range: 5/6/7 MEASURED on unit #1, 4/8/9
PROVISIONAL ("channels disagree 25-48 %"), all scaled by
`SCOPE_CAL_SOURCE_SCALE` = 0.92 from a JDS6600. How do they compare with my
unit and a source known to ±1 to 2 %? Does the generator leak into the other
input? How accurate is the device's frequency badge on a known tone?

## 2. Hypotheses

- Gain: ranges 5..7 agree with the table within the source's accuracy; 4/8/9
  may not. Falsifier for 5..7: a difference beyond ~3 %.
- Crosstalk: a 3 V, 1 kHz square on CH1 leaves no 1 kHz component on open CH2
  above its noise. Falsifier: the 1 kHz component on CH2 rises when GEN runs.
- Badge: error within EXP-17's acceptance (≤ 2 %, ≤ 0.32 % above bin 16).

## 3. Procedure

Scripts and raw data: `reverse_engineering/captures/exp83/` (`loop.py`).

- Gain: `loop.py gain`: per range 4..9, GEN DC at 0.5..3.0 V and at code 20,
  3 captures each, mean counts; railed points (227) excluded; fit counts
  against volts.
- Crosstalk: 30 captures of both channels with GEN off, then a 1 kHz 3.0 V
  square; Hann-windowed projection at 1 and 3 kHz (`xt.py` from EXP-79).
- Badge: `loop.py badge`: 0x0E/0x0F/0x10, four tones each, `fpga scope freq`
  (10 estimates), against the generator's real frequency.

## 4. Control

| control | expected | measured | passed? |
|---|---|---|---|
| tone through the crosstalk estimator on CH1 | square: fundamental 4/π × 73.6, 3rd at 1/3 | 93.7 and 31.3 counts | yes |
| gain fit linearity on ranges 5..9 | residual ≲ 0.5 count | 0.20 to 0.40 counts | yes |

## 5. Results

### Gain (CH1)

| range | measured mV/count | table × 0.92 | measured / table | tier | zero (counts) |
|---|---:|---:|---:|---|---:|
| 4 | 7.85 (2 points; rails above 1 V) | 12.95 | 0.61 | provisional | 46.7 |
| 5 | 20.01 | 20.08 | 0.996 | measured | 49.9 |
| 6 | 39.79 | 39.51 | 1.007 | measured | 46.6 |
| 7 | 78.90 | 81.35 | 0.970 | measured | 44.9 |
| 8 | 194.9 | 256.7 | 0.76 | provisional | 44.2 |
| 9 | 404.2 | 324.0 | 1.25 | provisional | 43.8 |

The measured steps are 2.55, 1.99, 1.98, 2.47, 2.07: the 2.5 / 2 / 2 / 2.5 / 2
cadence of a 1-2-5 attenuator. At stock's 25 px divisions these are 0.2, 0.5,
1, 2, 5 and 10 V/div. Stlkv's figures from a different unit and rig were
20 / 40 / 80 / 200 / 400 mV/count for ranges 5..9 and ~8 for the most
sensitive row (CLAUDE.md, pin table): agreement within 1 to 3 %.

The zero (GEN at code 20 ≈ 0.05 V) sits at 44 to 50 counts on every range,
the EXP-77 open finding, not mid-scale 128.

### Crosstalk

| | CH1 1 kHz | CH1 3 kHz | CH2 1 kHz | CH2 3 kHz |
|---|---:|---:|---:|---:|
| GEN off | 0.031 | 0.029 | 0.028 | 0.031 |
| GEN 1 kHz square, 3 V | 93.7 | 31.3 | 0.037 | 0.034 |

CH2 stays at its noise floor: crosstalk below about −68 dB (0.04 %) into an
open input.

### Frequency badge (`fpga scope freq`)

| code | gen Hz | device Hz | error | error with EXP-81's rate | bin |
|---|---:|---:|---:|---:|---:|
| 0x0E | 406.987 | 403.925 | −0.75 % | −0.62 % | 8.3 |
| 0x0E | 1323.043 | 1320.108 | −0.22 % | −0.09 % | 27.1 |
| 0x0E | 3409.091 | 3406.453 | −0.08 % | +0.06 % | 69.9 |
| 0x0E | 7692.308 | 7683.201 | −0.12 % | +0.02 % | 157.6 |
| 0x0F | 203.995 | 202.600 | −0.68 % | −0.61 % | 8.3 |
| 0x0F | 660.975 | 660.009 | −0.15 % | −0.07 % | 27.1 |
| 0x0F | 1704.545 | 1704.196 | −0.02 % | +0.06 % | 69.9 |
| 0x0F | 3846.154 | 3843.768 | −0.06 % | +0.02 % | 157.6 |
| 0x10 | 102.002 | 101.310 | −0.68 % | −0.60 % | 8.3 |
| 0x10 | 330.989 | 330.405 | −0.18 % | −0.10 % | 27.1 |
| 0x10 | 851.970 | 851.868 | −0.01 % | +0.06 % | 69.8 |
| 0x10 | 1924.619 | 1923.827 | −0.04 % | +0.03 % | 157.7 |

12 of 12 answered (10 of 10 estimates each). The error depends on the bin, not
the code: −0.6 % at bin 8, −0.1 % at bin 27, within ±0.06 % from bin 70. That
is the estimator's own low-bin bias; the rest is the table's rate (EXP-81).
Within EXP-17's acceptance.

## 6. Blind spots

- CH2's gain was not measured (the cable was on CH1).
- Range 4 rests on two points; ranges 0..3 rail.
- Absolute gain carries EXP-80's ±1 to 2 %.
- Crosstalk into an open CH2 at 1 kHz only; not at 50 kHz, not into a
  terminated input.

## 7. Conclusion

- **Established:** on my unit CH1 is 7.85 / 20.01 / 39.79 / 78.90 / 194.9 /
  404.2 mV per count on ranges 4..9, a 1-2-5 ladder that matches Stlkv's unit.
  The table's measured rows 5 and 6 agree within 1 %, row 7 is 3 % high, and
  the provisional rows 4, 8, 9 are wrong by 25 to 65 %. No measurable
  crosstalk from GEN into CH2. The badge is within EXP-17's acceptance; its
  remaining error is a low-bin bias of ~−0.6 % at bin 8.
- **NOT established:** CH2's gain; ranges 0..3.
- **Follow-up:** whether to write these rows into `scope_cal.c` is a decision
  for the user: the file's rule is one source scale and no row edits, which
  these numbers (another unit, another source) do not fit as is. CH2 with the
  cable moved.
