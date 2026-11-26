# EXP-80: the generator's absolute level, after the Rigol's Self-Cal

- **Date:** 2026-11-26
- **Unit:** my unit
- **Build:** `guest-coldtrace` from `feat/remote-control` 8c3caaa (build `Nov 25 2026 23:59:30`)
- **Instrument:** Rigol DS1104Z, 00.04.05.SP2, **Self-Cal run by the user this
  morning**; CH1 10x probe on GEN, ground clip on the 2C53T's ground
- **Status:** CONFIRMED. GEN's DC transfer is 0.7937 mV per DAC code (±0.2 %
  across four Rigol settings), intercept within ~20 mV of 0. Absolute
  uncertainty about ±1 to 2 % (probe ratio and Rigol gain, not checked
  against a second reference).

## 1. Problem

EXP-79 left the generator's absolute level unresolved because the Rigol failed
its own offset control: its DC reading moved by 0.2 V (100 mV/div) to 1.36 V
(500 mV/div) between bandwidth limit on and off. The loopback (EXP-83) needs
GEN in volts to check the scope's vertical scale.

## 2. Hypothesis

After Self-Cal the Rigol agrees with itself across BW-limit settings within
about one 8-bit level, and GEN's DC level is `code × 3.3/4095` within the
Rigol's accuracy. Falsifier: BW on/off still disagree by more than ~20 mV
(then no absolute number is possible), or GEN differs from nominal by more
than the Rigol's spread.

## 3. Procedure

Scripts and raw results: `reverse_engineering/captures/exp80/`.

1. `ctrl80.py`: GND coupling at five scale/offset settings, BW 20 MHz and off;
   then four GEN DC levels, each with BW on and off at one setting.
2. `null.py 20M` / `null.py off`: GEN DC at 9 codes, Rigol offset centred on
   the level, corrected by the GND-coupled reading at the same setting.
3. `dc0.py`: GEN DC (waveform 9) at amplitude 0..30 (codes 20..3742), Rigol
   offset 0 (its zero reads +2 to +37 mV there), at 1 V/div (whole range) and
   500 mV/div (up to code 2253), BW on and off, averaging 16, `vavg` × 3.

## 4. Control

| control | before Self-Cal (EXP-79) | after | passed? |
|---|---|---|---|
| same GEN level, BW on vs off, 100 mV/div (code 20) | 0.260 vs 0.042 V | 0.0579 vs 0.0556 V | yes |
| same, 500 mV/div (code 640 / 1881) | +0.772 vs -0.583 V | 0.5576 vs 0.5405 / 1.5473 vs 1.5313 V | yes (17 mV, < 1 level) |
| same, 1 V/div (code 3742) | | 2.9849 vs 3.0037 V | yes (19 mV) |
| GND, 500 mV/div, offset -1.75 V | +39 mV | +38 / +23 mV | within spec (±78 mV) |
| GND, 2 V/div, offset -1.4 V | +157 mV | +83 / +148 mV | within spec (±223 mV) |
| GND, 100 / 500 mV/div, offset 0 | | +0.9 to +19 mV | yes |

## 5. Results

`dc0.py`, fits over codes 144..3742 (the first point of each sweep is excluded:
it is read just after a coupling change and a 3 V step, while the 16-sweep
average still holds old frames, and reads 0.23 to 0.41 V):

| Rigol setting | mV per code | x 4095 | intercept | max residual | code 3742 ("3.0 V") |
|---|---:|---:|---:|---:|---:|
| 1 V/div, BW 20 MHz | 0.79295 | 3.2471 V | -6 mV | 6.4 mV | 2.961 V |
| 1 V/div, BW off | 0.79230 | 3.2445 V | +20 mV | 6.3 mV | 2.985 V |
| 500 mV/div, BW 20 MHz | 0.79494 | 3.2553 V | +35 mV | 5.7 mV | 3.010 V |
| 500 mV/div, BW off | 0.79453 | 3.2536 V | +18 mV | 4.4 mV | 2.991 V |

Mean slope **0.7937 mV/code** (spread 0.33 %), full scale 3.250 V, so the
DAC's reference (VDDA) is about 3.25 V rather than 3.3 V. The "3.0 V" DC
setting delivers 2.99 V (2.96 to 3.01). Unit #2's Fluke read 3.002 V at the
same setting.

The null measurement agrees within its own limit: the GND correction at 1 V/div
moves in 40 mV steps (a grounded input has no noise to dither the 8-bit
converter), so two of its rows differ by 57 mV between BW settings while all
others agree within 5 mV.

The +0.25 V offset of EXP-79 is gone: code 0 reads 0.035 to 0.039 V, code 20
0.050 V.

## 6. Blind spots

- Probe ratio (10x, typically ±1 to 2 %) and the Rigol's gain are shared by
  every setting, so the 0.33 % agreement does not bound them. No second
  reference (a DMM) was available.
- GEN into the probe's 10 MΩ only; no load test.
- Temperature and drift: one morning.

## 7. Conclusion

- **Established:** GEN = 0.7937 mV × code + (−6 … +35 mV), linear to ≤ 6.4 mV,
  on my unit, absolute to about ±1 to 2 %. The Rigol after Self-Cal agrees
  with itself across BW settings within one 8-bit level.
- **Excluded:** an offset on GEN (EXP-79's +0.25 V was the uncalibrated Rigol).
- **NOT established:** absolute accuracy better than ~±1 to 2 %.
- **Follow-up:** use 0.7937 mV/code as the loopback source scale (EXP-83).
