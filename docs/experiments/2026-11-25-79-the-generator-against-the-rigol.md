# EXP-79: the generator on DAC2, measured with a Rigol DS1104Z

- **Date:** 2026-11-25 (evening, about 23:15 to 23:55)
- **Unit:** my unit
- **Build:** `make guest-coldtrace` from `feat/siggen-dac2` (`feat/remote-control`
  4d98cfa + deOliveira-R's 4bf87c0 and b671235 cherry-picked as 3ca73da / 3a9f327,
  + 0d97f93 for the remote keys), build `Nov 25 2026 23:25:05`, 664,520 B,
  crc32 `48693735`, sha256 `c152bef1f3ff51ca…`, flashed over CDC (`cdc_flash.py`)
- **Instrument:** Rigol DS1104Z, firmware 00.04.05.SP2 (updated earlier the same
  day, **Self-Cal not run since**), CH1 with a 10x probe on the GEN output,
  driven with the `rigol` CLI (rigol-go) over LAN
- **Status:** CONFIRMED for frequency, shape, duty and flatness to 50 kHz.
  **Absolute level UNRESOLVED**: the Rigol's offset calibration failed its own
  control (section 4), so no absolute DC or amplitude number in this file is
  better than about ±3 %.

## 1. Problem

Our generator never reached the GEN jack. `dac_output.c` drove DAC1/PA4, which
is the scope's CH1 offset reference, and in `guest-coldtrace` the output was
held inert for that reason. Issue #68 (mquerostudio, unit #3) and deOliveira-R
(unit #2) showed that GEN is driven by **DAC2/PA5**, paced by TMR7 and fed by
DMA2 ch4, as in stock. Does that port, carried onto the fork, put the right
signal on GEN on my unit? We need to know before the generator can serve as
the known source for validating the scope (loopback).

## 2. Hypothesis

H1: with the DAC2 port, GEN carries the waveform the device says it built: the
frequency the device reports after TMR7 quantisation, the 100-sample cycle
the device holds in RAM, and the duty it was set to.

- Frequency falsifier: the ratio Rigol counter / device-reported frequency
  changes with frequency by more than the counter's resolution. A crystal
  offset between the two instruments moves every point by the same ratio; an
  error in the TMR7 maths does not.
- Shape falsifier: after a best-fit gain, offset and phase, the residual
  against the device's own cycle exceeds a few percent of span on smooth waves.
- Duty falsifier: the measured duty differs from the cycle's high fraction by
  more than one DAC step (1 %).

H2: the level is `code × 3.3 / 4095` V (unit #2 read 3.002 V on a Fluke at
DC 3.0 V). Falsifier: a measured DC transfer whose slope or intercept differs
by more than the instrument's accuracy.

## 3. Procedure

Device: `openscope shell "siggen wave W amp DV freq HZ duty PCT on|off [dump]"`
(the shell command from 4bf87c0; it works in any mode, so the device stayed in
scope mode with capture running). `dump` returns the 100 DAC codes of the cycle,
and the status line returns the real TMR7 frequency.

Rigol: CH1 DC, 10x, BW limit 20 MHz (unless stated), edge trigger on CH1 with
HF-reject coupling. Scripts: `reverse_engineering/captures/exp79/exp79.py`
(`freq`, `dc`, `shapes`, `duty`, `bw`), `null.py`, `noise.py`, `hf_shapes.py`,
`zero.sh`, `xt.py`. Raw results are the JSON files next to them (the crosstalk
capture CSVs, 30 records per state, were not kept). The scripts call `openscope`
and `rigol` from PATH; `OPENSCOPE=<path>` overrides the first.

- **freq:** sine 3.0 V at 22 frequencies, 1 Hz to 50 kHz, including values that
  TMR7 cannot hit exactly (1234, 12345, 33333, 49999). Rigol hardware counter
  (`:MEAS:COUN:VAL?`), 4 readings each.
- **dc:** waveform DC (index 9) at amplitude 0..30 (0.1 V steps), averaging 16,
  `vavg` × 3, at one fixed Rigol setting (500 mV/div, offset -1.75 V).
- **shapes:** all 13 waveforms at 1 kHz, 3.0 V, duty 50 %, averaging 64,
  100 µs/div, screen waveform (1200 points). Fit: the device's 100-sample cycle
  as a zero-order hold, phase searched in 0.1-sample steps, gain and offset by
  least squares; RMS and max residual reported.
- **duty:** square, 1 kHz, duty 0..100 % in 5 % steps, Rigol `pduty`.
- **bw:** sine 3.0 V, 10 Hz to 50 kHz, `vpp` at ~4 cycles per screen.

**Preconditions verified by readback:**

| what | expected | measured |
|---|---|---|
| running image | build 23:25:05 | `openscope info`: `OpenScope Nov 25 2026 23:25:05` |
| DAC2 on PA5 | PA5 analog, ch2 TMR7-triggered + DMA | PA5 CFG nibble 0x0; DAC CTRL ch2 `0x1015`; DMA2 ch4 PADDR `0x40007414`, CTRL `0x06B1` |
| scope's CH1 reference untouched | DAC CTRL ch1 `0x003D`, D1ODT 2048 | `0x003D`, 2048 (before and after the session) |
| TMR7 at 1 kHz | DIV 0, PR 2399 | DIV 0, PR 2399, CTRL1 `0x0001` |
| scope capture alive while the generator runs | `capture.gen` advancing | 90344 → 90568 in 3 s |
| Rigol | DS1104Z, CH1 10x | `rigol info`: DS1104Z, 00.04.05.SP2, CH1 10x |

## 4. Control

| control | expected | measured | passed? |
|---|---|---|---|
| path: generator on, 1 kHz sine 3.0 V | a 1 kHz sine on Rigol CH1 | 1 kHz sine, counter 1000.0538 Hz (screenshot `sine-1k-3v0.png`) | yes |
| path: generator off | the sine disappears | flat level, board noise only (`off-new-fw.png`) | yes |
| counter repeatability | stable readings | 3 of 3 identical at 1 kHz; spread ≤ 2 mHz at 50 kHz | yes |
| Rigol zero, GND coupling, offset 0 | 0 V | +0.5 mV (500 mV/div), 0 (200 mV/div), +4 mV (100 mV/div) | yes |
| Rigol zero, GND coupling, offset -1.75 V (500 mV/div) | 0 V | **+39 mV** | marginal (spec ±0.1 div ±2 mV ±1.5 % of offset ≈ ±78 mV) |
| Rigol zero, GND coupling, offset -1.4 V (2 V/div) | 0 V | **+157 mV** | marginal |
| same DC level, BW limit on vs off, 100 mV/div | same reading | **0.260 vs 0.042 V** (off state); 1.758 vs 1.542 V (code 1881) | **no** |
| same DC level, BW limit on vs off, 500 mV/div, offset -0.78 V | same reading | **+0.772 vs -0.583 V** (code 640), reproduced twice; the Rigol's own screen draws the trace at -584 mV | **no** |

The last two rows fail. The Rigol's DC level depends on the bandwidth-limit
setting by 0.2 V at 100 mV/div and 1.36 V at 500 mV/div. So **every absolute
DC or amplitude number is VOID** until the Rigol has run Self-Cal. Frequency,
duty, shape and amplitude ratios at one fixed setting do not depend on the
offset calibration and stand.

## 5. Results

### Frequency (H1): CONFIRMED

Counter against the frequency the device reports (`freq-233325.json`):

| request Hz | device reports Hz | Rigol counter Hz | ppm vs device |
|---:|---:|---:|---:|
| 2 | 2.000 | 2.000109 | +54.4 |
| 10 | 10.000 | 10.000543 | +54.3 |
| 37 | 37.000 | 37.001940 | +52.4 |
| 100 | 100.000 | 100.005495 | +54.9 |
| 1000 | 1000.000 | 1000.054100 | +54.1 |
| 1234 | **1233.933** | 1233.999900 | +54.2 |
| 10000 | 10000.000 | 10000.542000 | +54.2 |
| 12345 | **12371.134** | 12371.805000 | +54.2 |
| 25000 | 25000.000 | 25001.355500 | +54.2 |
| 33333 | **33333.333** | 33335.141000 | +54.2 |
| 49999 | **50000.000** | 50002.710500 | +54.2 |
| 50000 | 50000.000 | 50002.712000 | +54.2 |

All 21 points from 2 Hz to 50 kHz are at **+54.2 ppm** (±0.3; 37 Hz +52.4,
where the counter has the fewest periods per gate). The ratio is constant, so
the device's reported frequency is right, quantisation included: 12345 Hz really
comes out as 12371.134 Hz, which is 2171 ppm off the request and exactly where
the device says. The 54 ppm is the offset between the two instruments' crystals.
At 1 Hz the counter reads 0 (below its range); the screen measurement at 200 ms/div
gives period 1 s, +width 0.5 s, -width 0.5 s. Stock's own formula would give
2.203 Hz there (the device prints it alongside); the port's timing avoids that.

### Shape (H1): CONFIRMED

Against the device's own cycle, 1 kHz, 3.0 V (`shapes-234549.json`):

| wave | gain mV/code | RMS residual | % of span | max |
|---|---:|---:|---:|---:|
| Sine | 0.7885 | 13.0 mV | 0.44 | 99 mV |
| Square | 0.7859 | 58.0 mV | 1.98 | 975 mV |
| Sawtooth (50 % = triangle) | 0.7865 * | 17.6 mV | 0.60 | 50 mV |
| Half Wave | 0.7864 | 8.9 mV | 0.61 | 84 mV |
| Full Wave | 0.7876 | 10.9 mV | 0.74 | 43 mV |
| Step | 0.7861 | 37.2 mV | 1.27 | 433 mV |
| Rev Step | 0.7850 | 40.9 mV | 1.40 | 972 mV |
| Exp Rise | 0.7878 | 10.6 mV | 0.37 | 100 mV |
| Exp Fall | 0.7849 | 40.9 mV | 1.44 | 980 mV |
| DC | (flat, not fitted) | | | |
| Multi-tone | 0.7864 | 8.3 mV | 0.28 | 22 mV |
| Sinc Pulse | 0.7855 | 14.0 mV | 0.48 | 157 mV |
| Lorentz | 0.7880 | 10.6 mV | 0.36 | 83 mV |

\* fitted with negative gain and a half-period shift, which for a symmetric
triangle is the same curve.

Smooth waves fit to 0.3 to 0.7 % of span. The larger residuals belong to the
waves with a vertical edge (Square, Step, Rev Step, Exp Fall), and their maxima
of ~1 V are single samples on an edge. The gain is the same for all twelve
(0.7849 to 0.7885 mV/code, 0.5 % spread), so the output stage is linear across
waveforms.

At higher frequency (`hfshapes-234958.json`) the sine keeps its shape: 0.29 %,
0.41 %, 0.52 % of span at 10, 25, 50 kHz. The square's residual grows (4.5 %,
6.2 %, 9.9 %) because its edges are slew-limited: **rise 740 ns (10-90 %)**, a
straight ramp (`square-edge-10k.png`), about 3.2 V/µs. At 50 kHz an edge takes
~10 % of a half period. That is the DAC output buffer (or the GEN stage), not
firmware.

### Duty (H1): CONFIRMED

| set % | 5 | 10 | 15 | 20 | 25 | 30 | 35 | 40 | 45 | 50 |
|---|---|---|---|---|---|---|---|---|---|---|
| Rigol | 4.8 | 10.0 | 14.8 | 20.0 | 24.8 | 30.0 | 34.8 | 40.0 | 44.8 | 50.0 |

and symmetric up to 95 % (54.8, 60.0 … 94.8). 0 % and 100 % are flat, so the
Rigol has no duty to report. Every odd 5 % step reads 0.2 % low, which is one
2 µs screen sample at 200 µs/div, the measurement's resolution. Each duty step
on the device is one 10 µs DAC sample.

### Amplitude against frequency: flat

`vpp` 2.98 V at 10 and 100 Hz, 2.94 V from 1 kHz to 50 kHz (`bw-234914.json`).
The 40 mV step is one 8-bit level at 500 mV/div; nothing rolls off up to 50 kHz.

### DC transfer (H2): linear, absolute UNRESOLVED

At one fixed Rigol setting (`dc-233558.json`), 31 codes from 20 to 3742:
**0.79169 mV/code, intercept +0.277 V, R² 0.999995, max residual 8.1 mV.**
Linear to within the Rigol's resolution.

The intercept looked like a +0.25 V offset on GEN, since the off level read
0.251 V at 100 mV/div after the GND-coupled correction. **It is not established.**
With the BW limit off the same level reads 0.042 V. The Rigol's offset
calibration fails its control (section 4) by exactly this kind of amount. The slope
(3.24 V full scale for 4095 codes, against 3.3 V nominal) is inside the Rigol's
±3 % of full scale and also depends on the setting: a null measurement at
1 V/div gave 3.33 V for code 3742 where the fixed setting gave 3.24 V. Unit #2's
3.002 V on a Fluke stays the best absolute number available.

### Noise on GEN (weak evidence, see blind spots)

Normal acquisition, 100 mV/div: std 45 to 55 mV, peak-to-peak 0.32 to 0.58 V,
dominant component 9 to 10 MHz; the same with the generator off. A
mains-triggered average shows no 50 Hz component (30 mV residual).

### Crosstalk into the scope inputs (provisional, no positive control)

The 2C53T's own CH1 and CH2 on range 5, 30 captures each, with GEN off and then
driving a 1 kHz 3.0 V square. The 1 kHz component in the records:
CH1 0.027 → 0.028 counts, CH2 0.032 → 0.035 counts (peak), i.e. at the
estimator's floor, under ~1 mV at the BNC. The scope inputs carried whatever was
on them (CH1's probe clipped to its own ground, CH2 unknown).

## 6. Blind spots

- **Absolute level.** The Rigol had not run Self-Cal since its firmware update
  and failed its offset control. The 10x probe ratio (typically ±1 to 2 %) is
  in every absolute number too. No absolute DC or amplitude is established
  here.
- **Which crystal is off.** The 54 ppm is the ratio of two crystals; this test
  cannot say which one.
- **Load.** GEN was measured into the probe's 10 MΩ only. Output impedance and
  behaviour into 1 kΩ or 50 Ω were not tested.
- **Noise origin.** A 10x probe with a long ground lead next to a switching
  board picks up HF noise. The 45 mV std may be the probe's loop, not GEN.
- **The tables themselves.** The shapes were compared with the device's own
  cycle, so this shows "what the firmware built is what comes out". Whether the
  cycle equals stock's is `test-siggen`'s job (61 checks against stock's builder
  run in unicorn), not this test's.
- **Crosstalk** has no positive control in this session (the GEN-into-CH1
  loopback that would provide one needs a cable change).
- Only my unit; one evening; no temperature or long-term drift.

## 7. Conclusion

- **Established:** on my unit, the DAC2/PA5 + TMR7 + DMA2 ch4 generator puts on
  GEN exactly the frequency the device reports (constant +54.2 ppm against the
  Rigol from 2 Hz to 50 kHz, quantised values included), the 100-sample cycle it
  holds (12 of 12 shaped waveforms), the duty it is set to, and a flat amplitude
  to 50 kHz, while the scope keeps capturing and the CH1 reference on DAC1 stays
  untouched. Edges are slew-limited at ~740 ns.
- **Excluded:** a +0.25 V offset on GEN as a finding. It came from the Rigol,
  not from the device.
- **NOT established:** the absolute level (within ~±3 %), output impedance,
  and the absence of crosstalk.
- **Follow-up:**
  1. Run the Rigol's Self-Cal (30 min warm-up, inputs disconnected), then repeat
     `exp79.py dc` and `null.py` with both BW-limit settings; or read the DC
     levels with a DMM (or the 2C53T's own DCV input).
  2. Loopback: GEN into the 2C53T's CH1 (and the Rigol on the same node). That
     gives the crosstalk test its positive control and makes the generator the
     known source for the scope's features.
