# EXP-91: the "wrong edge" records are real: the trigger locks onto one edge of our own generator

- **Date:** 2026-11-26
- **Unit:** my unit
- **Build:** `guest-coldtrace` `Nov 26 2026 23:06:35` (the charge-screen merge,
  EXP-90's AUTO fix in); the badge change on `23:22:30`
- **Setup:** GEN sine (amp setting 3 V) into CH2 by a BNC cable, CH1 open;
  CH2 range 5, timebase 0x08, trigger source CH2, level 19 (reg 0x08 = 151,
  fires at record 123), AUTO unless stated
- **Status:** CONFIRMED (the classifier is right; the edge locks to the
  generator); the Freq badge follows the active channel (CONFIRMED)

## 1. Problem

EXP-90 found AUTO frozen with CH1 off: the edge filter dropped every
triggered record as the wrong edge. Is the classifier wrong with CH1 off
(the converter routing changes, `fpga_set_channel_mask(2)`), or does the FPGA
really fire on the other edge every time?

## 2. Hypotheses

- H1, routing: with CH1 off the records come back laid out differently and
  the classifier misreads them. Predicts: CH1 on keeps records, CH1 off drops
  them; captured records do not cross the level near index 512 in the
  direction the classifier says.
- H2, lock: the FPGA fires on either edge (EXP-53), whichever comes first
  after it re-arms; our generator runs from the same crystal as the
  acquisition loop, so at a round frequency the re-arm meets the same phase
  every time and the same edge fires. Predicts: at 1000 Hz all classified
  records are one edge whatever CH1 does, requesting that edge keeps them
  all, and a frequency that does not divide the loop's timing (1013 Hz,
  997 Hz) gives both edges.

## 3. Procedure

`captures/exp91/`: `grab.py` (CH1 off, then on: 12 committed records each
and the edge-filter counters), `classify.py` (the firmware's
`src/dsp/trig_edge.c` built as a host library, run over captured records),
`lock.py` (edge-filter counters over 5 s per frequency and requested edge).

**Preconditions verified by readback:** state per capture in `ch1-*.json`
(`trig.code` 151, source ch2, range 5, 0x08); settings restored afterwards
(`settings diff: {}` against the snapshot taken before).

## 4. Control

| control | expected under H1 | measured | H1 survives? |
|---|---|---|---|
| CH1 on, same signal and settings | records kept | kept +0, dropped +11, unclassified +9 | no |

## 5. Results

The committed records (CH1 on or off, the same): CH2 falls smoothly from
about 171 to 85 counts across the 1024 samples and crosses 123 at index
~512. At 0x08 a record is 205 us, a fifth of the 1 kHz period: one slope, and
it is the falling one. The firmware's verdict FALLING is right. (On the host
the committed records classify UNKNOWN: they are already put in time order,
so there is no seam left to find; the firmware classifies the raw record.)

Edge-filter counters, 5 s each, CH1 on:

| GEN | edge requested | kept | dropped | unclassified |
|---:|---|---:|---:|---:|
| 1000 Hz | rising | 0 | 16 | 11 |
| 1000 Hz | falling | 14 | 0 | 12 |
| 1013 Hz | rising | 15 | 4 | 7 |
| 1013 Hz | falling | 5 | 13 | 8 |
| 997 Hz | rising | 14 | 8 | 4 |

At the autoset's timebase for 1 kHz (0x0D, 8 cycles per record), NORMAL:

| edge | commits in 5 s | kept | dropped | unclassified |
|---|---:|---:|---:|---:|
| rising | 27 | +8 | +0 | +19 |
| falling | 19 | +0 | +7 | +20 |

The lock is there too (it flips to rising), but most records hold both edges
and come back unclassified, are committed, and the display's soft trigger
aligns them to the chosen edge: NORMAL works with either edge.

## 6. Blind spots

- The mechanism (the re-arm meeting the same phase) is inferred from the
  frequency dependence; the re-arm instants were not measured against the
  signal.
- Only our own generator. An external source drifts against the crystal, so
  it should give both edges (as 1013 / 997 Hz do), but none was tried.
- No fix was tried. Randomising the handover read by a fraction of the
  signal's period would break the lock, but the acquisition loop polls on a
  1 ms tick, which cannot shift the phase of a 1 kHz signal at all; it would
  need a sub-millisecond busy-wait.

## 7. Conclusion

- **Established:** the edge filter's verdicts are right; CH1 being off has
  nothing to do with it (EXP-90's reading withdrawn: the same drops with CH1
  on). With our own generator at a round frequency, the FPGA fires on the
  same edge every time. It matters only when a record holds less than half a
  period (one edge per record): there NORMAL with the other edge waits, and
  AUTO shows the starved records (EXP-90's fix).
- **Excluded:** H1 (routing).
- **Not established:** that external signals never lock; a fix.

## Addendum: the Freq badge follows the active channel

Until now row 1 (Freq, Vpp, Vrms, Duty) and Per were CH1's whatever was
selected, so a signal on CH2 alone showed `--` for Freq (EXP-90's
screenshots). They now measure the active channel (CH1/CH2 select it, the
top-left label names it) in its colour, and the last slot is the other
channel's peak to peak (`CH1pp` / `CH2pp`); an active channel that is off
hands them to the other; the held Freq resets when the channel changes.

On my unit, GEN 1 kHz into CH2, 0x0D, range 6 (screenshots,
`captures/exp91/badges-*.png`, weak evidence):

| active | Freq | Vpp | Vrms | Duty | Per | other |
|---|---|---|---|---|---|---|
| CH1 (open) | -- | 80 mV | 16 mV | -- | -- | CH2pp 2.95 V |
| CH2 | 995.2 Hz | 2.95 V | 1.04 V | 50.2 % | 1.00 ms | CH1pp 80 mV |

2.95 Vpp / 2 sqrt 2 = 1.04 V: the sine's RMS, as it should be.
