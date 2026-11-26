# OpenScope 2C53T - Button & Navigation Manual

Physical buttons: **MENU**, **AUTO**, **SAVE**, **CH1**, **CH2**, **PRM**, **SELECT**, **UP**, **DOWN**, **LEFT**, **RIGHT**, **OK**, **MOVE**

Emulator keys: `M`=MENU, `A`=AUTO, `S`=SAVE, `1`=CH1, `2`=CH2, `P`=PRM, `Space`=SELECT, Arrows, `Enter`=OK

---

## Mode Switching

The device has 4 modes, cycled by pressing **MENU**:

```
Oscilloscope  -->  Multimeter  -->  Signal Generator  -->  Settings
      ^                                                       |
      +-------------------------------------------------------+
```

If you're inside a Settings sub-menu, **MENU** goes back to the top-level Settings list first, then cycles to Oscilloscope on the next press.

---

## Oscilloscope Mode

### Channel Controls

| Button | Action |
|--------|--------|
| **CH1** / **CH2** | Not the active channel: select it (UP/DOWN etc. act on it). Active channel: switch it off. Off: switch it on and select it. The last channel on stays on. |
| **SELECT** | Cursors: Off -> Vertical -> Horizontal -> Both -> Off (time view, see Cursors) |

AC/DC coupling and probe 1X/10X are in Settings -> Oscilloscope (CHn Coupling,
CHn Probe). Until 2026-11-26 CH1/CH2 cycled the coupling and SELECT the probe.
Switching a channel off or on also changes the FPGA channel routing (PC1/PC2),
and a trigger source on a channel that goes off moves to the other channel
(EXP-86).

### MOVE: what the arrow keys do

**MOVE** hands the four arrow keys to the next mode. Each mode owns both axes,
and the mode in force is always shown as a tag in the top row, left of the
trigger status, in the colour of what it moves (hidden while the trigger
panel is open: the panel has the keys then). Pressing MOVE also raises a
popup naming the keys.

| Mode (tag) | UP/DOWN | LEFT/RIGHT | SELECT |
|------------|---------|------------|--------|
| **Scale** (grey, default) | V/div of the active channel | Time/div | Cursors |
| **Trigger** (trigger colour) | Trigger level (the dotted line, drawn denser in this mode) | Trigger point across the screen | Cursors |
| **Position** (channel colour) | Vertical position of the active channel | Trigger point across the screen | Cursors |
| **Mask** (warning colour, only while a mask exists) | Mask vertical tolerance ±2 counts | Mask horizontal tolerance ±1 sample | Stop-on-fail on/off |
| **Cursors** (cursor colour, only while cursors are on) | Move the active cursor 4 px | Next cursor | Cursors |

MOVE cycles Scale -> Trigger -> Position -> Mask -> Cursors -> Scale, skipping
Mask and Cursors when they are not there. Turning the cursors on with SELECT
hands them the arrows (tag: Cursors); MOVE takes the arrows back while the
cursors stay on screen, and switching the cursors off returns the arrows to
Scale. Clearing the mask does the same for Mask.

Position and the trigger point move half a grid division per press (13 rows,
16 columns); the popup shows the offset in divisions. The vertical position
is a display offset, applied to the trace, the trigger level line, the mask
bounds and the channel's "1"/"2" flag together, up to 5 divisions either way.
With both channels on, each starts in its own half of the screen, but a moved
channel can cross into the other half and overlay the other trace, as on a
bench scope; only the readout rows at the bottom and the top of the screen
stop it (a trace moved past them flattens there). The position is not saved
across power cycles. The H cursors' dV is read on the active channel's scale
(the one CH1/CH2 selected), wherever its trace sits. No arrow press blanks the screen; after a
Scale change the measurements below the trace refresh from the first capture
at the new setting (EXP-88).

**On the screen:**

- **Trigger point:** the small triangle under the top row of labels. It is in
  the trigger colour where the trigger locked, and grey at the requested
  column when nothing locked (a flat signal, free run). In Trigger and
  Position modes a dotted line runs down from it, crossing the trigger level
  line.
- **Trigger level:** the dotted horizontal line with the arrow at the right
  edge, denser in Trigger mode. A level above or below the whole signal sits
  on the band edge, which is where NORMAL mode holds. In Trigger mode UP/DOWN
  raise a popup with the level in volts at the source channel's probe tip,
  e.g. `Trig -1.08V  CH1` (EXP-89); on a range without a calibration it
  shows the level step and the FPGA code instead.
- **Channel references:** a "1" / "2" flag in the channel colour on the left
  edge of each channel's band, at that channel's 0 V (EXP-89: the measured
  zero for its range and offset setting). It moves with Position, and when
  0 V is outside the window the trace is drawn in, it sits on the band edge
  on the side 0 V is. In AC coupling 0 V is the signal's average. Without a
  zero (an offset reference never armed) it marks the middle of the fitted
  range instead.

The V/div and time/div labels come from the measured tables (`scope_cal.c`,
`scope_timebase.c`); a step without a measurement shows `--`.

**Vertical scale.** By default every trace is drawn at the V/div its label
shows: one grid division is that many volts, and 0 V sits on the middle of
the channel's half of the screen (the middle of the screen when only one
channel is on), at its flag. A grounded probe draws as a flat line (the
converter's 1-count noise is under one row). Settings > Oscilloscope >
Vertical Scale switches to **Autofit**, which stretches each trace to fill
its half whatever its size; the grid then means position only, and the
converter's noise shows as a comb on a quiet input (up to 4x magnified). A
range without a measured V/div (ranges 0 and 1) always autofits. Saved.

Until 2026-11-26 the stages split the axes: "Trig level" left LEFT/RIGHT on
the timebase, "Position" left UP/DOWN on V/div, there was no vertical
position, and cursors took all four arrows whenever they were on.

### Trigger panel

**TRIGGER** (time view) opens the trigger panel, stock's "Trigger Settings":

| Key while the panel is open | Action |
|-----|--------|
| **UP/DOWN** | Pick the row: Mode, Edge, Channel |
| **LEFT/RIGHT**, **OK** | Change it: Mode Auto / Normal / Single, Edge Rising / Falling, Channel CH1 / CH2 |
| **TRIGGER**, **MENU** | Close the panel |
| any other key | Closes the panel and does nothing else |

Channel is the trigger source, written to the FPGA (reg 0x06, EXP-86): the
hardware fires on that channel. A channel that is off cannot be the source
("CH2 is off"). The edge is an MCU filter on the source channel's record
(the FPGA fires on either edge, EXP-55). The panel and its changes draw
without blanking the screen. Mode and edge are also in Settings ->
Oscilloscope. In the FFT views TRIGGER still cycles the mode.

| Button | Action |
|--------|--------|
| **OK** | Toggle Run / Stop (or, while a mask failure is held, resume testing) |

### Mask pass/fail (time view)

| Button | Action |
|--------|--------|
| **AUTO** | No mask: teach one from the next 8 good captures. Teaching: cancel. Mask ready: turn it off |
| **MOVE** -> *Mask* | Adjust tolerances and stop-on-fail (MOVE table above) |
| **OK** | Resume after a held failure |

Every capture is then judged PASS / FAIL, or skipped with a reason (for example,
a setting changed since teaching). Dotted lines show the mask. Columns that
broke it turn red, and a strip along the bottom shows the verdict. The badge
row shows `P<n> F<n>`, `MASK skip: <why>`, or `FAIL ... HOLD-OK` while a
failure is held. Full design: `docs/specs/scope/mask-pass-fail.md`.

### Cursors

| Button | Action |
|--------|--------|
| **SELECT** | Cycle cursor mode: Off -> Vertical -> Horizontal -> Both -> Off (time view only; was TRIGGER until 2026-11-26). Turning them on hands them the arrows (MOVE mode Cursors) |
| **UP/DOWN** | Move the active cursor 4 px (UP = left / up) |
| **LEFT/RIGHT** | Select the next cursor; both keys do the same: V1 -> V2 (vertical), H1 -> H2 (horizontal), V1 -> V2 -> H1 -> H2 (both) |
| **MOVE** | Give the arrows back to Scale / Trigger / Position while the cursors stay on; MOVE again cycles back to Cursors |

The arrows move the cursors only in MOVE's Cursors mode (the tag says so). In the FFT, split, waterfall and XY views the arrows keep their view's meaning even with a cursor left on. The active cursor is solid, the others dashed. The readout sits at the top left under the top row: `dt=...  1/dt=...` for the vertical pair and `dV...` for the horizontal pair (in pixels when the two H cursors span both channel bands). Labels: V1/V2 at the bottom of the trace area, H1/H2 at the right edge. Cursors draw without blanking the screen (EXP-85).

### FFT / Spectrum Views

| Button | Action |
|--------|--------|
| **PRM** | Cycle view: Time -> FFT -> Split -> Waterfall -> Time |
| **SELECT** | Cycle FFT window function (in FFT/Split/Waterfall views) |
| **UP/DOWN** | Adjust reference level (+/- 5dB, in FFT views) |
| **LEFT/RIGHT** | Zoom in/out frequency range (in FFT views) |
| **AUTO** | Auto-configure FFT (in FFT views; in the time view AUTO is the mask key) |

Note: Entering an FFT view claims the shared memory pool (88KB). Returning to time view releases it.

### Run Control

| Button | Action |
|--------|--------|
| **OK** | Toggle acquisition Run / Stop (shows popup) |

---

## Multimeter Mode

The four buttons directly under the screen are **softkeys**: the bar along the
bottom edge of the screen says what each one does, right above it, and its
current value. Softkeys arrived with the softkey UI (2026-10-06; spec
`docs/specs/platform/softkey-ui.md`).

### Softkeys

| Button | Big / Graph / Stats | Limits | Fuse |
|--------|---------------------|--------|------|
| **MOVE** | **Function**: next function | Function | **Cal leads**: hold the tips together and press, to take the lead offset off every reading. On the Types page: **Fuse type** |
| **SELECT** | **Reset**: min/max/avg, chart, histogram | **Low**: arrows move it; press again = set to the reading | **Rating** (next, wrapping; arrows step). In Scan: **Draw if >** presets 0.1 / 0.2 / 0.5 / 1 / 2 mV |
| **TRIGGER** | **Relative** on/off (zeros at the current reading). In Continuity: **Beep** threshold 10 / 30 / 50 / 100 Ohm | **High**, likewise | **Show**: Detail -> Table -> Scan -> Types |
| **PRM** | **View**: Big -> Graph -> Stats -> Limits -> Fuse | View | View |

### Other buttons

| Button | Action |
|--------|--------|
| **OK** | **Hold**: freeze the reading on screen now (HOLD badge); OK again = live |
| **LEFT / RIGHT** | Previous / next function (Fuse view: fuse type) |
| **UP / DOWN** | Limits view: move the highlighted limit. Fuse view: rating up/down (Scan: threshold ±0.1 mV) |
| **AUTO** | Automatic function select (DC V / AC V) |
| **SAVE** | Raw-frame debug overlay |

Changing function is instant: the reading shows dashes while the meter chip
switches in the background (~1.5 s in the `-meter` image). In the plain
`coldtrace` image only DC volts is measured; other functions show `---`.

### 11 Functions

```
0: DC Voltage       6: Resistance
1: AC Voltage       7: Continuity  (short = below the Beep threshold, default 30 Ohm)
2: DC Current (mA)  8: Diode Test
3: DC Current (A)   9: Capacitance
4: AC Current (mA) 10: Temperature
5: AC Current (A)
```

### 5 Views

- **Big** — the reading in 70 px digits, unit underneath, one line of min/max (or the relative reference, or SHORT/OPEN). Readable across the bench.
- **Graph** — compact reading, scrolling strip chart with an auto-scaled Y axis.
- **Stats** — min/max/avg/peak-to-peak and a histogram of the readings.
- **Limits** — pass/fail: a big PASS / FAIL box, the reading's position on a Low–High bar, pass and fail counts, and the last failing value. Entering it with a live reading sets Low/High to the reading ±5 %.
- **Fuse** — fuse current tester: probe the two metal tips on top of a fuse
  left in place, and it estimates the current from the voltage drop and the
  fuse's typical resistance (about ±10 %). Entering it switches to DC V.
  **Calibrate the leads first:** touch the probe tips together and press
  **Cal leads**. The meter reads about −1 mV with nothing connected, which
  would otherwise show as a ~120 mA draw on a 10 A fuse. The value is kept
  until power-off. No verdict is shown until the reading has been steady for
  about a second ("unsteady: probes on?", or WAIT in Scan).
  - **Detail**: a picture of the selected fuse, the current, a 0–150 mA bar
    with the 50 mA parasitic-draw mark, the drop and the fuse resistance.
  - **Table**: every rating of the type, largest at the top, with the current
    each would mean.
  - **Scan**: DRAW / NO DRAW against the threshold, for walking a fuse box
    with the engine off, plus the current if the fuse is 10/15/20/30 A
    (20/30/40/60 A for Maxi and J-Case).
  - **Types**: "which fuse is this?" — the five types to scale, front and top,
    and the common rating colours. The number on top of the fuse is what
    counts; colours vary.

---

## Signal Generator Mode

### Waveform & Output

| Button | Action |
|--------|--------|
| **SELECT** | Cycle waveform type |
| **OK** | Toggle output ON / OFF |

### Waveforms

```
Sine -> Square -> Triangle -> Sawtooth -> Full-Rect Sine -> Half-Rect Sine -> Pulse -> Noise
```

### Parameters

| Button | Action |
|--------|--------|
| **UP** | Increase amplitude (+0.1 Vpp) |
| **DOWN** | Decrease amplitude (-0.1 Vpp) |
| **LEFT** | Decrease duty cycle (-5%) |
| **RIGHT** | Increase duty cycle (+5%) |
| **PRM** | Cycle frequency presets: 1 -> 10 -> 100 -> 1k -> 10k -> 25k -> 1 Hz |

---

## Settings Mode

### Top-Level Menu

```
> Oscilloscope Settings    [sub-menu]
  Sound and Light
  Auto Shutdown
  Display Mode             [cycles theme]
  Math / Persist           [sub-menu]
  Component Tester         [sub-menu]
  Bode Plot                [sub-menu]
  Startup on Boot
  About                    [info screen]
  FPGA SPI Scanner
  Firmware Update          [USB HID bootloader]
  Factory Reset
```

### Navigation

| Button | Action |
|--------|--------|
| **UP** | Move selection up |
| **DOWN** | Move selection down |
| **OK** | Enter sub-menu or toggle selected option |
| **MENU** | Back to top-level (from sub-menu) or cycle to Oscilloscope mode |

### Display Mode (OK to cycle)

```
Dark Blue -> Classic Green -> High Contrast -> Night Red -> Dark Blue
```

### Startup on Boot (OK/LEFT/RIGHT to cycle)

```
Scope <-> Meter
```

Fresh or erased settings default to `Meter`: the application starts in
Multimeter mode and configures the DMM frontend immediately after FPGA
initialization. Selecting `Scope` keeps the normal oscilloscope startup path.

### Oscilloscope Settings Sub-menu (depth 1)

```
> CH1 Coupling      [DC / AC / GND]
  CH1 Probe         [1X / 10X]
  CH1 20M Limit     [ON / OFF]
  CH2 Coupling      [DC / AC / GND]
  CH2 Probe         [1X / 10X]
  CH2 20M Limit     [ON / OFF]
  Trigger Mode      [Auto / Normal / Single]
  Trigger Edge      [Rising / Falling]
  Vertical Scale    [V/div / Autofit]
  Debug Info        [Off / On]
```

Press **OK** on any item to cycle its value. **MENU** to go back. The list
scrolls: seven rows fit, so Trigger Edge and Debug Info come into view as the
selection moves down (before 2026-11-26 Trigger Edge was never drawn).

- **Vertical Scale** (default V/div, saved): V/div draws each trace at its
  labelled volts per division with 0 V at its flag; Autofit fits each trace
  to its half (see "Vertical scale" above).

- **Debug Info** (default Off, saved): shows developer readouts on the scope
  screen. Today that is the `SPI3:OK n` chip in the top row: n counts the
  captures read from the FPGA that passed the checks, so a climbing number
  means acquisition is running (it stops in STOP and while NORMAL/SINGLE
  wait for a trigger). `SPI3:-- n [XX]` means reads timed out (XX = the
  first byte seen), `SPI3:wait` that the FPGA is up but nothing has arrived,
  `SPI3:off` that the FPGA is not initialised. The shell's `display` and
  `status` commands report the same counter.

### Math / Persist Sub-menu (depth 3)

```
> Math Channel      [Off / A+B / A-B / A*B / A/B / Max / Min]
  Persistence       [On / Off]
  Back
```

- **Math Channel**: OK cycles through math operations, then back to Off
- **Persistence**: OK toggles on/off. Enabling persistence claims the shared memory pool (evicts FFT if active)

### Component Tester (depth 4)

| Button | Action |
|--------|--------|
| **SELECT** | Cycle component type |
| **OK** | Enter Resistor Calculator |

### Resistor Color Band Calculator (depth 5)

Interactive graphical resistor with 4 color bands (Digit 1, Digit 2, Multiplier, Tolerance).

| Button | Action |
|--------|--------|
| **LEFT/RIGHT** | Move between bands |
| **UP/DOWN** | Change selected band's color |
| **SELECT** | Simulate measurement and show pass/fail |
| **OK** | Back to Component Tester |

Calculates expected resistance from color bands, then compares against measured value showing deviation percentage and PASS/FAIL against the selected tolerance.

### About Screen (depth 2)

Shows device info, pool status, heap free, license. **MENU** or **OK** to go back.

---

## Quick Reference Card

```
+--------+------------------+------------------+------------------+------------------+
|  Key   |   Oscilloscope   |    Multimeter    |    Signal Gen    |     Settings     |
+--------+------------------+------------------+------------------+------------------+
| MENU   | -> Multimeter    | -> Signal Gen    | -> Settings      | Back / -> Scope  |
| CH1    | Select / off / on|        -         |        -         |        -         |
| CH2    | Select / off / on|        -         |        -         |        -         |
| UP     | By MOVE mode *   |        -         | Amplitude up     | Selection up     |
| DOWN   | By MOVE mode *   |        -         | Amplitude down   | Selection down   |
| LEFT   | By MOVE mode *   | Prev function    | Duty cycle down  |        -         |
| RIGHT  | By MOVE mode *   | Next function    | Duty cycle up    |        -         |
| OK     | Run/Stop         | Hold             | Output on/off    | Enter / Toggle   |
| PRM    | Cycle FFT views  | Softkey: View    | Freq presets     |        -         |
| SELECT | Cursor mode      | Softkey (Reset)  | Cycle waveform   | Cycle comp type  |
| MOVE   | Arrow-key mode   | Softkey (Func.)  |        -         |        -         |
| TRIGGER| Trigger panel    | Softkey (Rel.)   |        -         |        -         |
| AUTO   | FFT auto-config  | Auto function    |        -         |        -         |
| SAVE   | Screenshot       | Debug overlay    | Screenshot       | Screenshot       |
+--------+------------------+------------------+------------------+------------------+

* Scope time view, by the MOVE mode shown in the top-row tag: Scale =
V/div (UP/DOWN) and time/div (LEFT/RIGHT, LEFT = faster); Trigger = level and
trigger point; Position = vertical position and trigger point; Mask =
tolerances; Cursors = move / next cursor. Other scope views keep their own
arrow meanings.
```
