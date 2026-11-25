# EXP-78: pressing UP resets the LCD controller

- **Date:** 2026-11-25
- **Unit:** bench unit #1
- **Build:** `make guest-coldtrace` from `feat/remote-control`, flashed over CDC:
  `exp78-lcd.bin` (readback, build 21:47:07), then `exp78-heal.bin`
  (+ recovery, build 22:08:06, sha256 `145c2730…`)
- **Status:** CAUSE LOCATED to the panel getting a hardware reset on a
  physical press of UP; root cause (mechanical or electrical) NOT established.
  Firmware recovery built; its positive control passes; the physical-press
  check of the recovery is pending.

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
| physical UP press | **pending** (expect recoveries +1, last found 0x08 / 0x00) |

## 6. Blind spots

- "Mechanical" is inferred by elimination (the UP net and the firmware path
  are excluded); no one has looked at the board. An ESD or ground-bounce path
  through the user's finger is not excluded either.
- Driving PC13 low tests the MCU side of the UP net; a switch whose other side
  is not ground, or a second contact in the same key, would not be seen.
- Whether stock firmware shows the same white screen on this unit is unknown
  (stock might re-initialise the panel on its own).

## 7. Conclusion

- **Established:** the white screen is the ST7789V in its hardware-reset
  state after a physical UP press; the MCU, the drawing and the LCD bus are
  fine.
- **Excluded:** firmware panel commands; the range relays via the firmware
  path; the UP net pulled low; the LCD pin configuration.
- **NOT excluded:** the root cause (mechanical flex of the board / LCD FPC,
  ESD). Worth a look with the case open: the LCD connector and what sits
  under the UP key.
- **Follow-up:** physical-press check of the recovery; try other keys and
  pressing the case near UP without actuating it.
