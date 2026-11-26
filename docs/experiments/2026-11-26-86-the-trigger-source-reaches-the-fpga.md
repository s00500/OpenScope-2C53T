# EXP-86: the trigger source reaches the FPGA; trigger panel and channel keys

- **Date:** 2026-11-26
- **Unit:** my unit
- **Build:** `guest-coldtrace`, this change (`Nov 26 2026 16:46:00`)
- **Setup:** GEN into CH2 by a BNC cable (CH2 x1, DC, range 5); CH1 open (AC,
  range 5); timebase 0x08; generator 5 kHz sine, 1.0 V
- **Status:** CONFIRMED

## 1. Problem

The trigger source was UI-only. `trig.source ch2` (remote) set the label and
the display's soft trigger, but nothing wrote FPGA reg 0x06 after boot, so the
hardware kept firing on CH1: with a signal on CH2 alone, NORMAL never fired
(handoff 2026-11-26, checked at 0x08 and 0x0E). The user also asked for
TRIGGER to open a trigger panel (stock's "Trigger Settings"), the cursors to
move to SELECT, AC/DC to stay in the menu, and channel on/off on CH1/CH2.

## 2. Hypothesis

Writing the source to reg 0x06 (0x00 CH1, 0x01 CH2) moves the hardware
trigger to that channel, as stock does (op06 = ms[0x16], 0x0800C890) and as
EXP-72 and deOliveira-R's f4dfc23 found. With a level only CH2 crosses:
source CH1 gives no NORMAL commits, source CH2 gives commits. Falsifier: CH2
as the source commits nothing, or CH1 commits.

## 3. Procedure

Change (from f4dfc23's design, adapted; their branch is not merged here):

- `fpga_apply_trigger_source()`: reg 0x06 under the acquisition park, the code
  in force recorded; boot reconcile writes it from the restored state; the
  edge filter classifies the source channel's record.
- `fpga_apply_channel_enables()`: channel on/off sets the PC1/PC2 routing mask
  (it only followed at the next V/div change before) and moves a source off a
  switched-off channel. Applied on scope entry and from the remote
  `chN.enabled` / `trig.source` (both used to be state-only).
- Trigger panel (`ui/trig_menu.c`, pure, `test-trig-menu`) drawn as band chips
  (EXP-85), so it never blanks the screen; trigger mode/edge/source moved to
  the live epoch. CH keys: `chan_key_decide()`. SELECT cycles the cursors.
- Shell: `fpga scope source [ch1|ch2]` prints the state and reg 0x06.

Instrument: `captures/exp86/commits.sh`, records committed per 10 s
(`fpga.spi3_ok_count`, `0x2000f1f8` in this build). In NORMAL a record is
committed only on a handover, so this counts triggers.

Levels, read back with `openscope capture`: CH1 48-50 counts (quiet), CH2
102-153 with the generator on, 100-103 with it off. `trig.level 20` =
code 0x98, which fires at record 124 (code - 28, EXP-56): mid-swing on CH2,
unreachable for CH1 and for CH2 with the generator off.

**Preconditions verified by readback:**

| what | expected | measured |
|---|---|---|
| reg 0x06 code in force after each switch | 0x00 / 0x01 | `fpga scope source`: matched every time |
| trigger mode / code | normal, 0x98 | `openscope get` |
| channel mask before / after CH2 off / on | 3 / 1 / 3 | `fpga.channel_mask` 03 / 01 / 03 |

## 4. Control

| control | expected | measured | passed? |
|---|---|---|---|
| counter moves (AUTO, src CH1) | many | 354 / 10 s | yes |
| NORMAL can fire through this path (src CH1, level -41 = code 0x4E, crossing at 50, inside CH1's noise) | > 0 | 49 / 10 s | yes |
| first attempt at the control above | | ran in AUTO: the CLI took `-41` as a flag and changed nothing (358 / 10 s). **VOID**, redone with `--` | |

## 5. Results

NORMAL, level 20 (code 0x98):

| condition | reg 0x06 | commits / 10 s |
|---|---|---|
| A: source CH1 | 0x00 | **0** |
| B: source CH2 | 0x01 | **45** |
| A': source CH1 | 0x00 | **0** |
| B': source CH2 | 0x01 | **43** |
| N: source CH2, generator off | 0x01 | **0** |
| B'': source CH2, generator on | 0x01 | **44** |

Through the buttons on my unit:

| action | result |
|---|---|
| TRIGGER, DOWN x2, RIGHT (panel: Channel -> CH2), TRIGGER | reg 0x06 = 0x01, 44 commits / 10 s; screenshot `captures/exp86/panel-on-device.png` |
| CH2 (CH1 active) | CH2 selected |
| CH2 again | CH2 off: mask 3 -> 1, source flipped to CH1 (reg 0x00), 0 commits / 10 s |
| CH2 again | CH2 on and selected: mask 1 -> 3, source stays CH1 |

Emulator (`scripts/emu_shot.py`): opening the panel, changing rows and the
source, closing it and SELECT for the cursors made 0 full repaints; CH1 as the
last channel is refused with a popup.

## 6. Blind spots

- Only range 5 on CH2 and one level. The 28-code level offset was measured on
  CH1 (EXP-55/56); on CH2 it is assumed. A wrong offset would move where CH2
  fires, not whether.
- The edge filter on CH2 records was not exercised separately (NORMAL fired on
  a sine with Rising; Falling on CH2 not checked).
- Mask 1 / 2 routing with one channel on was read back, not measured on a
  signal (which converter feeds which buffer after the switch).
- SINGLE was not run.

## 7. Conclusion

- **Established:** reg 0x06 selects the hardware trigger channel (0x00 CH1,
  0x01 CH2), A/B/A/B and with a generator-off control; the panel and the
  channel keys drive it and the routing mask on my unit.
- **Excluded:** the CH2 commits coming from CH2's idle level (0 with the
  generator off).
- **NOT excluded:** a CH2-specific level offset; edge filtering on CH2.
- **Follow-up:** Falling edge and SINGLE on CH2; the 28-code offset on CH2.
