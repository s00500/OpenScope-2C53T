# EXP-81: every timebase code is the round ladder, to 50 ppm

- **Date:** 2026-11-26
- **Unit:** my unit
- **Build:** `guest-coldtrace`, `feat/remote-control` 8c3caaa (build `Nov 25 2026 23:59:30`)
- **Setup:** loopback, GEN into CH1 by a BNC cable; CH1 x1, range 5, DC,
  trigger AUTO; CH2 open
- **Status:** CONFIRMED for 0x06..0x14 (15 codes). 0x13 and 0x14 agree in the
  median but scatter (section 5).

## 1. Problem

`scope_timebase.c` holds rates measured on unit #1 with the ESP32 source:
0x0E..0x14 MEASURED, 0x0D PROVISIONAL, 0x0A..0x0C "unmeasurable with our
source" (it stopped near 4.5 kHz), 0x06..0x09 withdrawn as INCOHERENT.
PR #51 (unit #3) and #84 (unit #2) have since measured 0x06..0x0D with other
sources. EXP-79 showed our own generator puts out exactly the frequency it
reports (+54 ppm against the Rigol at every point). What are the rates on my
unit, measured against a source exact to the crystal?

## 2. Hypothesis

The FPGA divides a fixed clock, so each code's rate is a fixed number. If so,
fs = f_gen / (fitted cycles per sample) is the same for every tone on a code
(spread at the fit's precision, ~1e-5), and the codes follow the 2 / 2 / 2.5
cadence EXP-18 found. Falsifier: fs moves with the tone beyond ~1e-4, or the
codes do not form the cadence.

## 3. Procedure

`reverse_engineering/captures/exp81/loop.py rates`: per code, up to four tones
placed at 7.3 / 23.7 / 61.1 / 137.9 cycles per 896 samples (non-integer, so
no bin lock), limited to 1 Hz..50 kHz and below 0.4 fs; per tone 6 captures
(`openscope capture`, CH1, head skip 128 → 896 samples). Frequency: `siggen`'s
real TMR7 value. Fit: sine with free amplitude/phase/offset; a rotation seam
(the record can be a rotation of one continuous segment) is found by scanning
every split point with one shared frequency, and kept only if it cuts the
residual by 4x. On synthetic rotated, 8-bit records the fit recovers the
frequency to ~1e-5 at ≥ 20 cycles and 2e-4 at 3 cycles.

## 4. Control

| control | expected | measured | passed? |
|---|---|---|---|
| GEN reaches CH1 | the 1 kHz sine | 73.6 counts amplitude, 38 dB SNR, 0.020000 cycles/sample | yes |
| fit on synthetic records with a seam | frequency recovered, seam found | 24 of 24 seams found; worst 1.8e-4 (3 cycles) | yes |

## 5. Results

Median over all records of all tones (`rates-summary.json`). Rates are in
units of the MCU crystal, which the generator is derived from:

| code | fs, my unit (S/s) | vs ladder | tones | records | table (unit #1) | PR #51 (unit #3) | #84 (unit #2) |
|---|---:|---:|---:|---:|---:|---:|---:|
| 0x06 | 24,999,852.8 | -6 ppm | 1 | 6 | - | 24,849,896 (-0.60%) | 24,981,000 (-0.08%) |
| 0x07 | 12,499,400.7 | -48 ppm | 1 | 6 | - | 12,498,676 (-0.01%) | 12,494,000 (-0.04%) |
| 0x08 | 4,999,738.5 | -52 ppm | 1 | 6 | - | 4,990,070 (-0.19%) | 4,997,500 (-0.04%) |
| 0x09 | 2,499,889.2 | -44 ppm | 1 | 6 | - | 2,500,893 (+0.04%) | 2,498,800 (-0.04%) |
| 0x0A | 1,249,938.4 | -49 ppm | 2 | 12 | - | 1,249,691 (-0.02%) | 1,249,600 (-0.03%) |
| 0x0B | 499,972.0 | -56 ppm | 3 | 18 | - | 500,203 (+0.05%) | - |
| 0x0C | 249,987.2 | -51 ppm | 4 | 24 | - | 250,089 (+0.04%) | - |
| 0x0D | 124,993.3 | -53 ppm | 4 | 24 | 123,663 (-1.06%) | 124,968 (-0.02%) | - |
| 0x0E | 49,997.3 | -54 ppm | 4 | 24 | 49,930.1 (-0.13%) | 50,020.0 (+0.05%) | - |
| 0x0F | 24,998.6 | -55 ppm | 4 | 24 | 24,979.1 (-0.08%) | 25,009.0 (+0.04%) | - |
| 0x10 | 12,499.3 | -53 ppm | 4 | 24 | 12,490.0 (-0.07%) | 12,489.0 (-0.08%) | - |
| 0x11 | 4,999.7 | -52 ppm | 4 | 24 | 4,990.8 (-0.18%) | - | - |
| 0x12 | 2,499.9 | -56 ppm | 4 | 24 | 2,494.9 (-0.20%) | - | - |
| 0x13 | 1,250.0 | -37 ppm | 4 | 24 | 1,250.4 (+0.04%) | - | - |
| 0x14 | 500.0 | -10 ppm | 4 | 24 | 500.2 (+0.04%) | - | - |

(percentages in the last three columns: that source's value relative to mine)

From 0x07 to 0x12 every code is the round ladder minus 44 to 56 ppm, and on
one code the tones agree to 3e-6 to 1e-4 (`spread` in `rates-*.json`); 0x06
has one tone at 1.8 cycles per record. No seams were needed from 0x06 to 0x12.

**The offset is the crystals, and it says which one is off.** EXP-79 read the
generator (MCU crystal) at **+54.2 ppm** against the Rigol; here the FPGA's
sample clock reads **−52 ppm** against the generator. So the FPGA's clock and
the Rigol agree to a few ppm, and the MCU's crystal runs about 52 ppm fast. In
seconds, the rates are the round ladder to within ~5 ppm.

0x13 and 0x14 scatter: per-tone spreads of 5e-3 to 2.3, 1 to 2 records of 6 with
a seam, SNR 3 to 14 dB at 0x14. Their medians still land on 1,250.0 and 500.0.
At those rates one record spans 0.7 to 1.8 s, so an AUTO commit can mix
samples from two moments; NORMAL-trigger records would settle it (as #84 found
for the fast codes, where AUTO's two channel reads are 140 µs apart).

## 6. Blind spots

- One unit. The other units' numbers sit within their sources' accuracy of
  the same ladder, which is the cross-unit check.
- 0x06..0x09 rest on one tone each (the generator stops at 50 kHz).
- 0x00..0x05 not measured (beyond the generator).
- CH1 only, AUTO records, 896 samples after the head skip.

## 7. Conclusion

- **Established:** on my unit, 0x06..0x14 are 25 M, 12.5 M, 5 M, 2.5 M,
  1.25 M, 500 k, 250 k, 125 k, 50 k, 25 k, 12.5 k, 5 k, 2.5 k, 1.25 k and
  500 S/s, to about 50 ppm in MCU-crystal units and ~5 ppm in seconds. The
  table's measured rows are 0.07 to 0.20 % low, 0x0D 1.06 % low, and
  0x06..0x0C are missing.
- **Also established:** the MCU crystal of my unit runs ~52 ppm fast; the
  FPGA's sample clock does not share it.
- **NOT established:** 0x00..0x05; whether 0x13/0x14 scatter is the read path.
- **Follow-up:** the table could carry the ladder as measured values (a
  decision for the user: `test_scope_timebase.c` currently fails the build if
  rows are "tidied" to the round ladder, a guard written when nobody had
  measured them that precisely). PR #51 and #84 are the upstream homes for
  this evidence.
