/*
 * OpenScope 2C53T — per-channel, per-range vertical calibration
 *
 * WHAT THIS IS
 * ------------
 * One number per (channel, range): how many millivolts at the probe tip one
 * ADC count is worth. Everything vertical the instrument claims — Vpp, Vrms,
 * the volts/div in the status bar, cursor deltas — is that number times a
 * count. There is nothing else in the vertical path.
 *
 * WHERE THE NUMBERS COME FROM
 * ---------------------------
 * Since 2026-11-26: EXP-83 (CH1) and EXP-84 (CH2), my unit. The onboard
 * generator's DC levels into each input by a cable, its volts taken from a
 * self-calibrated Rigol DS1104Z (EXP-80). EXP-89 added ranges 2 and 3 and
 * re-measured 4; EXP-92 added 0 and 1 (and 2 again, at finer steps): now
 * 0-7 MEASURED, 8/9 PROVISIONAL, none NONE; scope_cal.c gives the per-range
 * evidence. The rest of this section, the tier text
 * included, is the history.
 *
 * Before: docs/experiments/2026-08-17-08-per-range-gain-ladders.md, bench unit #1.
 * Slope method: span measured at five drive amplitudes per range, mVpp
 * regressed against counts, so the constant noise-floor term lands in the
 * intercept instead of inflating the gain. n = 5 on every entry below.
 *
 * WHY THERE ARE TIERS
 * -------------------
 * The measurement is not equally good everywhere, and the previous version of
 * this table (two hardcoded constants in scope_ui.c) is the reason the
 * distinction is now structural rather than a comment. Those two constants
 * claimed 2.882 mV/count on range 2 and 6.494 mV/count on range 8. Range 2
 * rails on both channels and produces no usable span at all, and range 8
 * measures 279 mV/count — a factor of 43 out. They were plausible, stable,
 * and wrong, and they had been on the instrument face for days.
 *
 *   MEASURED     ranges 5/6/7. The two channels agree within 6%, the ladder
 *                is a clean 1-2-4 doubling, and the numbers reproduce BOTH an
 *                independent mux-code sweep on this unit (EXP-06: 21.23 /
 *                42.04 / 91.41) and Stlkv's measurements on a DIFFERENT unit
 *                with a different rig (20 / 40 / 80). Three methods, two
 *                units, one answer.
 *   PROVISIONAL  ranges 4/8/9. Measured the same way, but the two channels
 *                disagree by 25-48%, which is the signature of one fixed
 *                amplitude set under-serving both ends of the sweep: too
 *                small to move range 9, big enough to clip range 4. The
 *                number is the right order of magnitude and nothing more.
 *   NONE         ranges 0-3. Both channels rail. EXP-11 split this into two
 *                different faults: 0/1/2 sit at 255 even with NO input and
 *                after centring, so they are pinned rather than overdriven;
 *                range 3 is a genuine sensitive row (~5.6 mV/count) that
 *                clips because its offset cannot be centred — the injector
 *                runs out of travel. Range 3 is therefore a firmware problem,
 *                not a dead tap, and is the one worth chasing.
 *
 * A PROVISIONAL entry still returns a number, because "roughly 280 mV per
 * count" beats a raw count for a user trying to read a trace. A NONE entry
 * returns 0.0f and the caller MUST fall back to honest ADC counts. That
 * contract is the whole point: a confident wrong number is worse than a raw
 * count, and callers are not allowed to invent one.
 *
 * THE ABSOLUTE SCALE IS ONE MULTIPLY AWAY FROM CORRECT
 * ----------------------------------------------------
 * (History: true of unit #1's rows until 2026-11-26. The current rows trace
 * to a self-calibrated Rigol; see SCOPE_CAL_SOURCE_SCALE below. The
 * one-multiply contract still holds.)
 * Every number here traces back to an amplitude *commanded* from the ESP32
 * signal generator, which has never been checked against a calibrated source.
 * Its frequency readback is already known to be off by ~0.82x, so assuming
 * its amplitude is exact would be optimistic. If it turns out to be off by a
 * scale factor, every entry in this table is off by that same factor —
 * uniformly, because they were all taken through the same source on the same
 * evening.
 *
 * That is why SCOPE_CAL_SOURCE_SCALE exists and is applied to every entry at
 * lookup time rather than baked into the table. When a trusted source arrives
 * on the bench, measuring ONE range recovers the factor and correcting it
 * here rescales the whole instrument. Do not "fix" individual rows by hand
 * to make them agree with a reference; that would break the one property
 * that makes this recoverable.
 *
 * WHAT THIS TABLE IS NOT
 * ----------------------
 * - Not per-unit. These are one unit's numbers compiled into the image (my
 *   unit since 2026-11-26; bench unit #1 before), and two other units agree
 *   within 1-3 % on ranges 5-7.
 *   Factory per-device cal exists in MCU flash at 0x08006000 (see
 *   docs/experiments and the factory-cal memory note) and our app overwrites
 *   it. Wiring that up is a separate job.
 * - Not offset. This is gain only. The zero point is set by the per-channel
 *   vertical-offset reference (DAC1/PA4 for CH1, TMR13/PA6 for CH2) and is
 *   re-centered per range by `fpga scope center`.
 * - Not horizontal. There is no sample-rate table here yet. (The claim that
 *   "the capture does not resolve frequency at all" was WITHDRAWN by EXP-10 on
 *   2026-08-18 — it was a double FFT in the measuring script, not a hardware
 *   defect. The time axis is linear and measurable: R^2 = 0.9990 over
 *   100 Hz - 3.5 kHz, fs = 14,890 S/s at reg 0x01 = 0x10.) Seconds need the
 *   same treatment volts just got: measure the rate per reg-0x01 code, tier
 *   it, and refuse to print when there is no entry.
 */

#ifndef SCOPE_CAL_H
#define SCOPE_CAL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Number of frontend range indices. Matches VDIV_COUNT in scope_state.h; the
 * index passed here IS the frontend range index written to the attenuator
 * bank (`fpga_set_ch1_frontend_range`), not a display-only index. */
#define SCOPE_CAL_RANGE_COUNT   10u

/*
 * Counts per screen division, fixed by the renderer, NOT by the hardware.
 *
 * scope_ui.c maps a sample to y as SCOPE_MID_Y - (sample-128)*SCOPE_H/256 and
 * rules a horizontal grid line every 26 px, with SCOPE_H = 206. So one
 * division is 26 * 256/206 = 32.3 counts. 32 is that rounded, and it is also
 * 256/8 — a full ADC span across eight divisions, which is the convention the
 * grid spacing was chosen to produce.
 *
 * scope_ui.c carries a compile-time assertion that its geometry still agrees
 * with this constant, so changing the grid without changing this is a build
 * error rather than a silently wrong volts/div label.
 */
#define SCOPE_CAL_COUNTS_PER_DIV   32.0f

/*
 * Correction for the calibration source's true-vs-commanded amplitude.
 *
 * 1.0f since 2026-11-26. The rows are now absolute: they were measured against
 * the onboard generator, whose DC transfer a self-calibrated Rigol DS1104Z
 * read at 0.7937 mV per DAC code, the same within 0.33 % across four Rigol
 * settings (EXP-80). Absolute accuracy is about +-1-2 %, set by the probe's
 * ratio and the Rigol's gain, which no second reference has checked. If one
 * does, correct THIS constant, not the rows: every row came from that one
 * source in one session, so its error is uniform by construction.
 *
 * History: 0.92f (2026-08-28, EXP-20/21) corrected unit #1's ESP32-based rows
 * against a JDS6600. It does not apply to the new rows.
 */
#define SCOPE_CAL_SOURCE_SCALE     1.0f

typedef enum {
    SCOPE_CAL_NONE = 0,     /* unusable range — caller must show ADC counts */
    SCOPE_CAL_PROVISIONAL,  /* right order of magnitude, channels disagree   */
    SCOPE_CAL_MEASURED,     /* cross-validated three ways on two units       */
} scope_cal_tier_t;

/*
 * Millivolts per ADC count for one channel on one range, source-scale
 * applied. `ch` is 1 or 2. Returns 0.0f for an unusable range or an
 * out-of-domain argument — callers MUST treat 0.0f as "no calibration" and
 * fall back to counts.
 */
float scope_cal_mv_per_count(uint8_t ch, uint8_t range_idx);

/* Volts per ADC count AT THE PROBE TIP — the form the badges, cursor and
 * volts/div labels want. = mV/count x the channel's probe factor / 1000.
 * 0.0f as above. scope_cal_mv_per_count() above stays BNC-referred: it is the
 * calibration table (what `fpga scope cal` prints and the bench scripts
 * verify), and a probe setting must never look like a calibration change. */
float scope_cal_volts_per_count(uint8_t ch, uint8_t range_idx);

/* Probe attenuation, read through a registered source so the probe setting
 * has ONE home (scope_state) and ONE application point (here). Until
 * 2026-10-03 the 1X/10X setting reached nothing but an unobeyed USART bit:
 * a user on a 10x probe saw volts 10x too small. No source registered (host
 * tests, early boot) or a factor <= 0 means 1.0. */
typedef float (*scope_cal_probe_fn)(uint8_t ch);
void  scope_cal_set_probe_source(scope_cal_probe_fn fn);
float scope_cal_probe_factor(uint8_t ch);

/* How much to trust the above. */
scope_cal_tier_t scope_cal_get_tier(uint8_t ch, uint8_t range_idx);

/*
 * May the renderer draw this (channel, range) at TRUE vertical scale — i.e.
 * make one grid division mean exactly scope_cal_volts_per_div()?
 *
 * True-scale rendering fixes the sample->pixel slope at SCOPE_CAL_COUNTS_PER_DIV
 * counts per division; that is only honest when a volts/div actually exists for
 * the range. This returns nonzero exactly when it does (MEASURED or
 * PROVISIONAL). On a NONE range it returns 0 and the renderer MUST fall back to
 * its own-min/max autofit, whose graticule has no volts meaning — the same
 * counts-vs-volts contract every other consumer of this module honours.
 */
int scope_cal_true_scale_ok(uint8_t ch, uint8_t range_idx);

/*
 * Volts per screen division, derived from the measured gain and the
 * renderer's counts-per-division. This is the number the status bar should
 * show, because it is the number the grid actually means. Returns 0.0f when
 * uncalibrated.
 */
float scope_cal_volts_per_div(uint8_t ch, uint8_t range_idx);

/*
 * Status-bar label for a range, e.g. "1.37V", "699mV", "~8.9V" (provisional,
 * leading tilde) or "--" (uncalibrated). Always NUL-terminated. Writes at
 * most `n` bytes; 8 is enough for every entry.
 *
 * Deriving the label from the measurement is deliberate. The nominal
 * vdiv_table labels ("5mV" ... "5V") were never derived from anything: on the
 * three ranges we trust they are out by 2.7x to 3.5x, so a range labelled
 * "2V" was showing a 2.8 V division. A label the instrument cannot support is
 * the same class of defect as an invented measurement.
 */
void scope_cal_range_label(uint8_t ch, uint8_t range_idx, char *out, uint32_t n);

/* The same label at the BNC, ignoring the probe factor: for the calibration
 * dump (`fpga scope cal`), whose table must not change with a UI setting
 * (EXP-70 addendum: the first probe build moved that column). */
void scope_cal_range_label_bnc(uint8_t ch, uint8_t range_idx, char *out, uint32_t n);

/*
 * THE ZERO: where 0 V at the BNC lands in ADC counts (EXP-89, 2026-11-26).
 *
 * The gain above turns a DIFFERENCE of counts into volts. An absolute volt
 * (a trigger level, a DC level, a 0 V marker) also needs the count that
 * means 0 V. That count is set by the channel's vertical-offset reference,
 * DAC1/PA4 for CH1 and TMR13/PA6 for CH2, both 12-bit codes, and it moves
 * by a fixed amount per code that is the same on every range (the offset is
 * injected behind the attenuator):
 *
 *     zero(ch, range, code) = zero_ref[ch][range] + k[ch] x (code - 2048)
 *
 * Measured on my unit with the input shorted, DC coupling, 4 records per
 * point: zero_ref 40.7..48.8 counts, k = 0.1214 (CH1) / 0.1220 (CH2)
 * counts per code; the model meets every point within 0.29 counts, over
 * 2048..3072 (CH1's 1536 rails low). The zero
 * is known to about +-0.3 counts (the ranges disagree by that much, more
 * than their noise allows: a quiet 8-bit input barely dithers), about 1 % of
 * a division. In AC coupling the same count is the signal's average, which is
 * what an AC-coupled volt means. Per-unit, like the gains.
 */
#define SCOPE_CAL_ZERO_REF_CODE    2048u

/* 0 V in counts for this (channel, range) with the offset reference at
 * `offset_code`. False (and *out untouched) for an out-of-domain channel or
 * range. */
bool scope_cal_zero_counts(uint8_t ch, uint8_t range_idx, uint16_t offset_code, float *out);

/* A record value as volts AT THE PROBE TIP: (counts - zero) x volts/count.
 * False when the range has no gain (tier NONE) or the arguments are out of
 * domain: no absolute volt without both halves. */
bool scope_cal_counts_to_volts(uint8_t ch, uint8_t range_idx, uint16_t offset_code,
                               float counts, float *out);

/* The offset code that puts 0 V at `counts` on this (channel, range), rounded
 * and clamped to 0..4095. Out-of-domain arguments return the reference code. */
uint16_t scope_cal_offset_for_zero(uint8_t ch, uint8_t range_idx, float counts);

/* Volts as a short signed label, "+1.23V", "-45mV", "+12.3V", "0mV". Always
 * NUL-terminated; 8 bytes is enough below 1000 V. No %f in the firmware's
 * printf, so integer maths. */
void scope_cal_format_volts(float v, char *out, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* SCOPE_CAL_H */
