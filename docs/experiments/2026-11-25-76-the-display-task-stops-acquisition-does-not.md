# EXP-76: the display task stops; acquisition does not

- **Date:** 2026-11-25
- **Unit:** bench unit #1
- **Build:** `make guest-coldtrace` from `feat/remote-control` (PR #47 + M6
  control surface, uncommitted working tree), image sha256 `570b4c9f…`,
  flashed by the user through the factory IAP drive
- **Status:** CONFIRMED (part 3): the freeze was an infinite loop in the
  frequency estimator, `scope_freq.c` `analyse()`; fixed and confirmed on
  the device. The timebase write refusal is fixed and confirmed by A/B.

## 1. Problem

The user reports that the scope screen freezes after changing V/div and
timebase (first seen from the `openscope live` keys). Is that the
acquisition, the display, or the new remote-control path, and is it
pre-existing firmware behaviour?

## 2. Hypothesis

H1: a timebase or range write leaves the acquisition task not committing
(wedged FPGA cycle, frames rejected by the validity gate, or parked).
If H1 is true, after the change `SPI3 OK` (commits) stops moving, and either
`total timeouts` (rejects) climbs or poll reads stop. If H1 is false,
commits keep moving at the AUTO poll rate and successive records differ.

H2 (added after H1 was refuted): the display task stops rendering while
acquisition continues. If true, the LCD shadow stays byte-identical across
new commits, a forced full redraw and a mode switch. If false, at least the
mode switch repaints it.

## 3. Procedure

All writes through the M6 remote path (`openscope set` / `openscope mode`,
the same `SET_PARAM` / `SET_MODE` the live view sends, which call the same
`fpga_apply_timebase()` / `fpga_apply_vdiv()` as the device buttons).
Instruments, in `openscope-cli/.scratchpad/exp76/` (`sweep.sh`, `burst.sh`):

- rates over ~2 s from `shell status`: `SPI3 OK` (commits), `SPI3 timeouts
  (total …)` (rejected frames), `PC0 edges` (strobes), `poll reads (total …)`;
- `capture.gen` from `GET_STATE`;
- content: two distinct committed CH1 records via `GET_WAVEFORM`, compared;
- LCD: `screen dumpbin` via `openscope screenshot --no-hold --torn`, compared
  by hash (the shadow records every pixel written through `lcd_write_data()`;
  only XY and waterfall views bypass it).

Inputs: nothing connected (both channels flat, CH1 45..48 counts).

**Preconditions verified by readback:**

| what | expected | measured |
|---|---|---|
| mode | scope | scope |
| timebase / hw | 0x0E / 0x0E | 0x0E / 0x0E |
| CH1 / CH2 range | as left by the user | 6 / 4 |
| trigger | auto, level 0 | auto, level 0, code 0x80 |
| PC0 edges since boot | (unknown) | 0: no triggered record this boot |
| uptime | (unknown) | ~13 min at the first readout |

## 4. Control

| control | expected | measured | passed? |
|---|---|---|---|
| A: AUTO, 0x0E, r6 (acquisition alive) | commits ~33/s, records differ | 33.8/s, differ | yes |
| B: NORMAL with nothing crossing (a known freeze) | commits 0, reads continue | 0.0/s commits, 34.1/s reads, no 2nd record | yes |
| A': back to AUTO | as A | 33.5/s, differ | yes |
| LCD: AUTO live vs NORMAL hold (screenshot changes vs not) | live differs, hold static | all 6 byte-identical | **no: VOID** |

The LCD control could not show a live screen, so the screenshot instrument
proves nothing on its own here. It became evidence only through the
repaint tests in 5c, where the expected change is large (a label, a whole
mode screen), not a few noise pixels.

## 5. Results

**5a. Every timebase code and range, settled (1.5 s after the write):**
codes 0x00..0x14 at CH1 r6, and CH1 r0..r9 at 0x0E: commits 30.3..34.6/s,
rejects 0.0/s, strobes 0.0/s, records differ at every setting. No setting
stopped acquisition.

**5b. Back-to-back bursts (one write in flight, like a held key):**

| burst | writes | time | result | commits/s in the next 1.3 s, then |
|---|---|---|---|---|
| B1 timebase 0x0F..0x14..0x0E | 12 | 3.71 s | **one write NAK `HW_REFUSED`** | 23.8, then 32.7..33.7 |
| B2 CH1 range 7,8,9,8,7,6,5,4,5,6 | 10 | 0.48 s | all OK | 33.2..33.9 |
| B3 mixed timebase/CH1/CH2 | 12 | 1.46 s | all OK | 31.7..34.6 |

The refusal fits the code: after a timebase write the acq task re-primes,
does one read, then sleeps `fpga_acq_post_edge_get()` = one 1024-sample fill
+ 100 ms before polling (500.2 S/s at 0x14: 2147 ms; 0x13: 919 ms). The park
point is only at the loop top, and `fpga_acq_pause()` gives up after 1 s, so
a write that lands within that sleep is refused. Which step of B1 was refused
was not recorded.

**5c. The LCD:**
- The shadow showed `CH1:643mV` (CH1 range 5: 21.83 mV/count x 32 x 0.92
  source scale) while the device reported range 6, `H=641us` (0x0E),
  `CH2:~2.79V AC` (r4).
- Byte-identical (`174be69b…`) across: 3 shots in AUTO, 3 in NORMAL, CH1 set to
  r7 (remote op sends `DCMD_REDRAW_ALL`), 1 s later, back to r6, **a switch to
  meter mode** (whole-screen repaint), and back to scope.
- Meanwhile the input task completed both mode switches (replies received)
  and acquisition committed 31.4/s with differing records.

## 6. Blind spots

- The screenshot reads the shadow, not the panel. No one looked at the panel
  during this run.
- Whether the display task is blocked or spinning: it runs at priority 1, so
  a spin starves only idle, which nothing here observes.
- When the display stopped: before the first screenshot, at a moment when
  CH1 was at range 5. The user's session and this sweep (B2, B3, the range
  sweep) both passed through range 5, so this run may have caused it.
- A signal that crosses the trigger (PC0 strobes) was never present. The
  triggered path (post-edge sleeps after every handover) was not exercised.
- The guest watchdog is not armed (`watchdog_init()` is under
  `#ifndef GUEST_BUILD`), so its silence says nothing about task health.

## 7. Conclusion

- **Established:** acquisition keeps committing after every timebase code,
  every CH1 range, and back-to-back bursts (H1 refuted for flat inputs in
  AUTO). The display task stopped rendering while acquisition, the input
  task and USB kept running (H2 supported). Writes landing within one
  post-edge sleep after a change to 0x14 (and likely 0x13) are refused with
  `HW_REFUSED`. That is pre-existing (`fpga_acq_pause()` 1 s vs a sleep of up
  to 2.15 s) and affects the device buttons the same way (`H=… NOT SET`).
- **Excluded:** the M6 remote path as a separate cause. It calls the same
  `fpga_apply_*` functions as the buttons, and acquisition is unaffected
  either way.
- **NOT excluded:** that the display hang is triggered by something other
  than a setting change; that the panel shows something the shadow does not.
- **Follow-up:**
  1. A display-loop breadcrumb (stage id + loop counter) readable from the
     shell, so the next hang says where the task stopped.
     *Built 2026-11-25:* `src/ui/display_trace.{c,h}`, shell `display` (loop
     count, stage and its age, task state, stack free, saved PC / LR and
     code-like stack words decoded from the FreeRTOS ARM_CM4F context;
     decoder pinned by `make test-display-trace`, both frame shapes,
     mutation-checked).
  2. Re-run B2/B3 against the breadcrumb build until the shadow stops, with
     the panel watched. (`openscope-cli/.scratchpad/exp76/repro.sh`.)
  3. Let the post-edge sleep wake on a pause request (sleep in slices, or a
     task notification) so a park always acks within one poll.
     *Built 2026-11-25:* `acq_sleep_parkable()` in `fpga.c`, 10 ms slices,
     runtime `fpga parkwake on|off` (default on) so B1 can be run both ways
     in one session. Image sha256 `06a1fd97…`, 660,116 bytes.

## Part 2 (same day): where it stops, and why

**Instrument:** the breadcrumb image (sha256 `06a1fd97…`). Its positive
control is the counter itself: a healthy display loop completes a pass every
~50 ms.

**Result, first read after the user flashed it and the screen froze again:**

```
display loops 0  pass began 13905 ms ago  stage scope_render for 13896 ms  cmd 5  render 2
task ready  stack free 491 words  saved sp 0x2002A7A4
saved pc 0x08013176  lr 0x08031E8D  exc_return 0xFFFFFFED (fpu frame)
stack code words: 080133E5 0800B2BB 080086FB 08030A31
(2 s later: loops 0, pc 0x0801317C, same stack words)
Uptime: 0:00:18
```

The task never finished its first pass after boot, it is `ready` (preempted,
not blocked) and its PC moves: a spin, not a wait. `addr2line`:
`analyse()` scope_freq.c:144-145 <- `scope_freq_estimate()` :181 <-
`draw_measurement_badges()` scope_ui.c:688 <- `draw_scope_screen()` :2304 <-
`vDisplayTask()` main.c:757.

**Mechanism.** The power-spectrum loop starts at bin 1, so `fft_re[0]` keeps
the raw real DC term, which after the mean removal and the Hann window is
often negative. With the power peak at bin 1 the three-point interpolation
takes `sqrtf(fft_re[0])` = NaN, so `fbin` = NaN, `(uint32_t)(NaN * h)` = 0,
and the harmonic walk's only exit (`c + 1 >= half`) is never reached.
The estimator runs only when the timebase in force has a rate (0x0D-0x14),
which is why it looked tied to timebase changes.

**Host reproduction on real records:** 40 consecutive CH1 records read off
the stuck device (nothing connected, 0x0E, r6) through the UNMODIFIED
`scope_freq.c` on the host, each in a child with a 1 s alarm:
**13 hang, 27 refused**. Instrumented copy: all 13 hangs are `peak=1` with
`fft_re[0] < 0`; `peak=1` with `fft_re[0] >= 0` never hangs.

**History.** `sqrtf(fft_re[peak - 1u])` dates from `084f3f1` (2026-08-19,
v0.3.0) but was harmless while the harmonic walk used the integer peak. The
walk moved onto the interpolated `fbin` in `620eb02` (2026-10-01), which is
on `main` and in no release tag: v0.4.0/v0.4.1 cannot hang this way, any
current `main` build can.

**Fix** (`scope_freq.c`): bin 0 is converted to power like the others (still
outside the total and the peak search); an interpolated bin that is not in
`[0.5, half)` (which a NaN fails) is refused as "no usable peak"; the
harmonic walk is bounded at `2 * half` steps.

| check | result |
|---|---|
| the 40 real flat records, fixed code | 0 hang, 40 refused (correct: no signal) |
| `test_scope_freq` training set | 63 answered / 9 refused / 0 wrong, worst 1.2% (identical to before) |
| `test_scope_freq` held-out set | 52 / 8 / 0, worst 3.7% (identical) |
| new `test_flat_inputs_terminate` (synthetic bin-1 record + the 40 records as `tests/fixtures/scope_records_flat.txt`, under a 5 s alarm) | passes on the fix |
| same test against `git show HEAD:…/scope_freq.c` (negative control) | FAILS: "did not return within 5 s (synthetic bin-1 record)" |

Image with the fix: sha256 `88530f38…`, 660,156 bytes.

**Still open:** on-device confirmation (display loop counter advancing with
nothing connected at a measured timebase, then `repro.sh`); the `parkwake`
A/B of the B1 burst; whether anything else in the display path can still
stall (the breadcrumb stays in the image for that).

## Part 3 (same day): on-device confirmation, image `88530f38…`

Same unit, nothing connected, 0x0E, CH1 r6, AUTO: the exact condition that
froze the previous image within 18 s of boot (`loops 0`).

| check | result |
|---|---|
| user, looking at the panel | "looks a lot better" (screen observation: weak evidence) |
| `display` at uptime 2:18 | loops 1107, then 1128 two seconds later; pass 39 ms old |
| `repro.sh 30` (60 bursts, ~660 writes, timebase 0x0C-0x10, CH1 r4-r9, CH2 r4-r5) | no stop; loops 1403 to 1437 over the last rounds |
| B1 timebase burst, `fpga parkwake on`, x3 | 3/3 all 12 writes OK, 0.58 / 0.64 / 0.64 s |
| B1, `fpga parkwake off` (the old behaviour, control), x3 | 3/3 refused `HW_REFUSED`, 3.83 / 3.80 / 3.77 s |

The control for the stop detector is part 2 itself: the same counter showed
`loops 0` and a 13.9 s old pass on the unfixed image. The `parkwake off`
runs show the refusal is still reachable through the same path, so the 3/3
clean runs with it on are a result, not a blind instrument. (The parkwake
script's count of writes accepted before the refusal did not parse the CLI
output and printed 0; only "refused" is claimed.)

**Not shown:** a signal that crosses the trigger (PC0 strobes) was still
never present, so the triggered acquisition path and the estimator on a real
tone were exercised only by the host fixtures, not on the device.

**Side observation, not investigated:** the display loop ran ~8-10 passes
per second in scope mode here (1107 passes in 138 s), well under the
50 ms queue wait alone would allow; full redraws dominate.

**Upstream:** the estimator bug is on `main` (since `620eb02`) independent
of the remote-control work, so `scope_freq.c`, `tests/test_scope_freq.c`
and `tests/fixtures/scope_records_flat.txt` belong in their own small change
against `main`.
