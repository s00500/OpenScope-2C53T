# EXP-78: pressing UP resets the LCD controller

- **Date:** 2026-11-25
- **Unit:** my unit (the first commits of this session said "bench unit #1";
  that was wrong)
- **Build:** `make guest-coldtrace` from `feat/remote-control`, flashed over CDC:
  `exp78-lcd.bin` (readback, build 21:47:07), then `exp78-heal.bin`
  (+ recovery, build 22:08:06, sha256 `145c2730…`), then 2438e09 (+ the PD6
  fix cherry-picked from #85, build 22:58:44)
- **Status:** CONFIRMED. The panel's reset line PD6 (RESX) floats in
  OpenScope, and a physical key press pulses it low: issue #85 (deOliveira-R,
  2026-11-12) found and fixed this on unit #2; holding PD6 high ends it on
  my unit too. We should have searched the issues before the bench work.

## 1. Problem

After a few presses of the physical UP button the screen goes white and stays
white, while the device keeps running. Why?

## 2. Hypothesis

H1: the panel controller (ST7789V) lost its configuration (a hardware reset or
a supply dip), not the firmware's drawing. If true, the panel's own registers
read the reset state while the MCU's LCD shadow (`screen dumpbin`) shows a
normal frame. If false, the panel reads configured and the cause is elsewhere
(backlight, panel supply, bus).

## 3. Procedure

New shell commands (usb_debug.c):

- `lcd status`: RDDID 0x04, RDDST 0x09, RDDPM 0x0A (power), RDDMADCTL 0x0B,
  RDDCOLMOD 0x0C, read with a slowed XMC read strobe (DATAST 255 HCLK instead
  of boot's 4) that is restored afterwards; the display task is held while the
  shell owns the bus.
- `lcd reinit`: SWRESET, read, then SLPOUT/MADCTL/COLMOD/DISPON as boot, read.
- `lcd sleep`: DISPOFF + SLPIN (the control for the recovery below).

**Preconditions verified by readback:** none needed beyond the anchors in §4.

## 4. Control

| control | expected | measured | passed? |
|---|---|---|---|
| ID anchor, healthy screen | 85 85 52 (ST7789V) | 85 85 52 | yes |
| healthy screen state | power 0x9C, MADCTL 0xA0 | 0x9C, 0xA0, COLMOD 0x05 | yes |
| right after SWRESET (`lcd reinit`) | asleep, display off | 0x08, MADCTL/COLMOD kept (A0/05) | yes |
| after re-init | 0x9C | 0x9C | yes |

So the readback is real (anchored), and it tells a reset panel from a working
one. A software reset keeps MADCTL and COLMOD; a hardware reset returns them to
their defaults (0x00 / 0x06), which separates the two.

## 5. Results

| action | panel after |
|---|---|
| user: a few presses of the physical UP button, screen white | **power 0x08 (asleep, display off, booster off), MADCTL 0x00, COLMOD 0x06**, status 00 61 00 00, uptime unbroken (16:35), display task drawing (loops advancing) |
| `lcd reinit` | recovered: 0x9C, 0xA0, 0x05 (screen back) |
| CH1 through all ten ranges, both directions, via SET_PARAM (the same `fpga_apply_vdiv` the button calls), panel read after every step | 0x9C / 0xA0 / 0x05 at every step |
| UP/DOWN injected through the input queue (BUTTON command), bursts of 2-3 | 0x9C / 0xA0 / 0x05 |
| PC13 (the UP line: input pull-up, idles 1, a press shorts it to ground) driven LOW 0.3 s by the MCU, i.e. a press without the mechanics | 0x9C / 0xA0 / 0x05 (the button scan saw it: CH1 range 7 -> 9) |
| control for that: PC13 output HIGH | 0x9C / 0xA0 / 0x05 |

The white screen is the **hardware-reset signature** (MADCTL and COLMOD back to
power-on defaults), on a physical press only. The firmware sends no panel
commands after boot, the MCU did not reset, the range relays switched through
the same code path do not do it, and pulling the UP net low electrically does
not do it.

**Retraction during the session:** PD12 configured as a GPIO output (GPIOD
CRH `BB11BBBB`) was first suspected as the LCD's register-select line. It is
not: PD12/PD13 are the CH1/CH2 AC/DC coupling relays, GPIO outputs on purpose
since 2026-08-15 (fpga.c), and the LCD's RS is A16 on PD11, in AF mode as
required. The whole LCD bus (PD0/1/4/5/7/8-11/14/15, PE7-15) was correct.

**Recovery (firmware):** every 250 ms, between frames (no RAMWR open), the
display task reads RDDPM and MADCTL; two bad reads in a row (not awake + display
on, or MADCTL not the boot value) and it runs SLPOUT/MADCTL/COLMOD/DISPON as
boot and repaints. Counted in `lcd status` (checks, recoveries, last bad
values). Predicate host-tested with the bench values (`test_display_trace`).

| recovery check | result |
|---|---|
| boot, no fault | 17 checks, 0 recoveries |
| `lcd sleep` (0x08, MADCTL kept) | screen restored within 0.6 s: 0x9C, recoveries 1, last found 0x08 / 0xA0 |
| 5 s after, no fault | 59 checks, still 1 recovery (no false positives) |
| physical UP presses (user: "goes white every time ... and indeed recovers") | recoveries 8 after ~9.5 min (1 was `lcd sleep`); last found **0x0C / 0xA0**: asleep but display on and MADCTL set, i.e. an earlier recovery's SLPOUT was ignored (the ST7789 refuses SLPOUT for 120 ms after a reset) and the next check finished the job. So a press costs up to two recoveries, ~0.75 s of white. |

**A/B against the user's previous firmware.** The user reported it "did not
happen previously". The reference was `fix/continuity-pc3-beep` @ 9d1d135
(upstream main + the EXP-93 continuity beep, then numbered EXP-74), built as `guest-coldtrace` (build
22:24:34, 650,984 B), flashed over CDC: **it goes white on a physical UP press
too.** So none of this session's commits (M6, display trace, parkwake, vertical
map, panel readback) causes it. Next reference: the v0.4.1 release image
(`openscope-2c53t-v0.4.1-coldtrace.bin`, sha256 87d76d34..., matches the
release SHA256SUMS; installer passes the #42 check, so the CDC round trip is
safe).

**v0.4.1 release** (`openscope-2c53t-v0.4.1-coldtrace.bin`, build Oct 5
15:13:06, flashed over CDC): **goes white on a physical UP press too.** Three
firmwares, the same reset: not a regression.

**Root cause: PD6, the ST7789V's RESX, floats (issue #85).** Found by searching
the upstream issues after the A/B runs; #85 reports the same signature (RDDPM
0x08, COLMOD 0x06) after key presses on unit #2, shows stock V1.2.0 holding PD6
as a push-pull output, high (0x0802ABEC / 0x0802AC5A..0x0802AD0A), and confirms
PD6 = RESX functionally (PD6 low -> white). Our own GPIOD dump earlier in this
session already showed it: CRL `B4BB44BB`, PD6 nibble 4 = floating input (read
1: the line idles high but nothing holds it).

| step on my unit | result |
|---|---|
| live, from the shell: PD6 output latch set high (`mem write 0x40011410 0x40`), then `gpio mode D6 out` (no low glitch) | PD6 CRL 3, odt 1; panel untouched (0x9C, 0 recoveries) |
| user presses UP several times | "no white anymore"; **4337 checks, 0 recoveries** (before: 1-2 per press) |
| cherry-pick of #85's 2c68e37 as 2438e09 (PD6 held high in the boot LCD block, write cycle 17 ns -> 67 ns), flashed | at boot PD6 = out PP high, SNWTCFG0 low half 0x0A06; panel 0x9C, 0 recoveries; display loop 9 passes/s before and after |

## 6. Blind spots

- WITHDRAWN 2026-11-25: "Mechanical" was inferred by elimination. The
  elimination never covered PD6, because which pin is RESX was not known here
  (it was, in #85). Root cause above.
- An ESD or ground-bounce path through the user's finger is what couples into
  the floating RESX; that coupling path itself was not measured.
- Driving PC13 low tests the MCU side of the UP net; a switch whose other side
  is not ground, or a second contact in the same key, would not be seen.
- Whether stock firmware shows the same white screen on this unit is unknown
  (stock might re-initialise the panel on its own).

## 7. Conclusion

- **Established:** the white screen is the ST7789V in its hardware-reset
  state after a physical UP press; the MCU, the drawing and the LCD bus are
  fine.
- **Excluded:** firmware panel commands; the range relays via the firmware
  path; the UP net pulled low; a regression (three firmwares incl. v0.4.1).
- WITHDRAWN 2026-11-25: "the LCD pin configuration" was listed as excluded.
  Only the bus pins were checked; the reset pin PD6 was floating. And "NOT
  excluded: mechanical flex / ESD" as the open root cause: superseded by
  PD6 (#85).
- **Established (final):** PD6 floating is the cause; holding it high fixes
  it on my unit (0 recoveries over many UP presses). The self-heal stays as a
  safety net; for upstream, #85's `pr/lcd-health-check` covers the same.
- **Follow-up:** none for this symptom. Lesson: search the issue tracker
  before the bench.
