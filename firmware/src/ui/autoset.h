/*
 * OpenScope 2C53T: autoset decisions (the AUTO key in the scope's time view).
 *
 * Stock's AUTO is an autoset (scope_autoset_trigger_track); ours was the mask
 * key until 2026-11-26, when the user asked for the autoset and moved the
 * mask to the trigger panel. With calibrated volts (EXP-83/84/89) and true
 * scale as the default view, an autoset can choose real settings:
 *
 *   vertical   per channel, the most sensitive range on which the signal
 *              fits the converter (6..215 counts at that range's 0 V) and
 *              the screen (at most `target_div` divisions peak to peak),
 *              then a Position that puts its middle on its half's middle
 *   timebase   from the signal's cycles per sample, the measured code that
 *              shows about AUTOSET_SCREEN_CYCLES cycles on the 320 columns
 *   trigger    the level at the middle of the source signal
 *
 * Everything here is arithmetic on a record and the calibration tables: no
 * hardware, no RTOS, no buffers (the input task's stack is 1.5 KB and RAM is
 * tight), host-tested by tests/test_autoset.c. The state machine that applies
 * the decisions and waits for fresh captures is tasks/scope_autoset.c.
 */
#ifndef AUTOSET_H
#define AUTOSET_H

#include <stdbool.h>
#include <stdint.h>

#define AUTOSET_CEIL           226u   /* both channels clip at 227 (EXP-89) */
#define AUTOSET_ADC_LO           6.0f /* the signal must sit inside these    */
#define AUTOSET_ADC_HI         215.0f /* counts on the range it gets          */
#define AUTOSET_SCREEN_CYCLES    3.0f /* cycles on the 320 columns            */
#define AUTOSET_MIN_PP_COUNTS    6u   /* below this a record is noise or DC   */
#define AUTOSET_SIGNAL_VPP      0.020f /* a signal: >= 20 mV peak to peak      */
#define AUTOSET_SIGNAL_VDC      0.050f /*        or a DC level >= 50 mV        */

typedef struct {
    uint8_t lo, hi;      /* 0.5 % and 99.5 % levels: one glitch cannot set them */
    float   mean;
    bool    clipped;     /* >= 0.5 % of samples at 0 or at AUTOSET_CEIL+       */
} autoset_stats_t;

/* Levels, mean and clipping of n samples (no buffer: counting passes). */
void autoset_stats(const uint8_t *s, uint16_t n, autoset_stats_t *out);

/* Cycles per sample from rising crossings of the middle of [lo, hi] with
 * hysteresis of an eighth of the span (at least 2 counts). 0 when the span
 * is under AUTOSET_MIN_PP_COUNTS or fewer than two crossings were seen. */
float autoset_cycles_per_sample(const uint8_t *s, uint16_t n, uint8_t lo, uint8_t hi);

/* The most sensitive calibrated range (2..9) on which [vmin, vmax] volts at
 * the BNC fit the converter and at most target_div divisions, with the
 * channel's offset reference at offset_code. 9 when none does. */
uint8_t autoset_pick_range(uint8_t ch, uint16_t offset_code,
                           float vmin, float vmax, float target_div);

/* The measured timebase code showing about AUTOSET_SCREEN_CYCLES cycles of
 * a hz signal on the screen; 0xFF for hz <= 0 (keep the timebase). */
uint8_t autoset_pick_timebase(float hz);

/* Position (rows up) that puts the count `mid` on the middle of the
 * channel's half, true scale, 0 V at `zero`: clamped to +/-MOVE_VPOS_MAX. */
int16_t autoset_position_rows(float mid, float zero);

/* The UI trigger level whose reg 0x08 code fires at record count `mid`
 * (code = mid + 28, EXP-55/56; code = 128 + level * 256 / 206, as
 * fpga_trigger_code_from_level). */
int16_t autoset_trigger_level(float mid);

/* Is there anything to set up for: a swing or a DC level, in volts. */
bool autoset_has_signal(float vpp, float vmean);

#endif /* AUTOSET_H */
