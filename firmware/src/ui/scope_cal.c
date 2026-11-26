/*
 * OpenScope 2C53T — per-channel, per-range vertical calibration
 *
 * See scope_cal.h for provenance, tiers, and the source-scale contract.
 * This file is deliberately free of hardware and RTOS dependencies so it can
 * be exercised on the host — see tests/test_scope_cal.c.
 */

#include "scope_cal.h"

#include <stdio.h>

/*
 * mV per ADC count at the BNC (x1), my unit, 2026-11-26.
 * Source: docs/experiments/2026-11-26-83-loopback-ch1-gain-crosstalk-and-the-
 * badge.md (CH1) and -84- (CH2): the onboard generator's DC levels through a
 * cable, 6 levels per range, slope of counts against volts; the generator's
 * volts from a self-calibrated Rigol (EXP-80, 0.7937 mV per DAC code, absolute
 * to about +-1-2 %). Railed points excluded, so range 4 rests on two levels.
 * Both channels in one session from one source, so SCOPE_CAL_SOURCE_SCALE is
 * 1.0 and still the one place to correct that source later.
 *
 * WITHDRAWN 2026-11-26: bench unit #1's rows (EXP-08, ESP32 source, x0.92
 * from a JDS6600), which these replace:
 *     CH1  14.08  21.83  42.95  88.42  279.05  352.17   (r4..r9, raw)
 *     CH2   9.49  20.96  41.71  83.79  223.71  425.00
 * Scaled by 0.92, rows 5/6 agreed with my unit within 1 %, row 7 was 3 %
 * high, and the provisional rows 4/8/9 were 25-65 % off.
 *
 * History, unchanged:
 * 0.0f means "this range produced no usable span at any drive amplitude" —
 * it is not a missing measurement, it is a measured refusal.
 *
 * WITHDRAWN 2026-08-18: the two values this table replaces, which lived as
 * bare constants in scope_ui.c's scope_cal_volts_per_count():
 *     range 2 -> 2.882 mV/count ("20mV/div", 347 codes per volt)
 *     range 8 -> 6.494 mV/count ("2V/div",   154 codes per volt)
 * Both are contradicted by the five-amplitude sweep: range 2 rails on both
 * channels and yields no span, and range 8 measures 279.05 (CH1) / 223.71
 * (CH2), a factor of 43 away. Kept here in writing rather than deleted,
 * because the failure mode — a plausible stable number nobody could tell was
 * wrong — is one this project has now hit four separate times.
 */
static const float mv_per_count[2][SCOPE_CAL_RANGE_COUNT] = {
    /*        r0     r1     r2     r3      r4      r5      r6      r7       r8       r9 */
    /* CH1 */ { 0.0f, 0.0f, 0.0f, 0.0f,   7.85f, 20.01f, 39.79f, 78.90f, 194.87f, 404.21f },
    /* CH2 */ { 0.0f, 0.0f, 0.0f, 0.0f,   7.82f, 19.86f, 39.39f, 77.11f, 206.53f, 430.48f },
};

/*
 * Confidence per range. Identical for both channels by construction: the
 * tier is a statement about how well the two channels AGREE, so it cannot
 * differ between them.
 *
 * 2026-11-26 (EXP-83/84, my unit):
 *   r4     PROVISIONAL  channels agree (7.85 / 7.82) but only two levels fit
 *                       below the rail, so there is no linearity check
 *   r5     MEASURED     channels differ by 0.8 %, slope SE 0.3-0.5 %
 *   r6     MEASURED     1.0 %, SE 0.2-1.0 %
 *   r7     MEASURED     2.3 %, SE 0.9-1.2 %
 *   r8     PROVISIONAL  5.8 %, SE 1.7-1.9 %: the generator's 3 V spans only
 *                       ~15 counts here
 *   r9     PROVISIONAL  6.3 %, SE 2.5-3.1 %: ~7 counts of span
 * The ladder steps 2.5 / 2 / 2 / 2.5 / 2 (a 1-2-5 attenuator) and matches
 * Stlkv's unit (20 / 40 / 80 / 200 / 400, and ~8 for range 4) within 1-3 %.
 *
 * Before (unit #1, EXP-08):
 *
 *   r0-r3  NONE         railed on both channels at every amplitude.
 *                       EXP-11 (2026-08-18) says more: 0/1/2 sit at 255 with
 *                       the input QUIET and after a centring pass, so they are
 *                       not sensitive rows being overdriven. Range 3 IS a
 *                       sensitive row (~5.6 mV/count, order of magnitude only)
 *                       and its defect is OFFSET, not gain — the injector hits
 *                       the bottom of its travel (DAC1 code 7, TMR13 code 3)
 *                       and still cannot pull the mean off 235-250, so the
 *                       waveform clips. Potentially fixable in firmware.
 *   r4     PROVISIONAL  channels differ by 48% (14.08 vs 9.49)
 *   r5     MEASURED     channels differ by 4.1%
 *   r6     MEASURED     channels differ by 2.9%
 *   r7     MEASURED     channels differ by 5.5%
 *   r8     PROVISIONAL  channels differ by 25%
 *   r9     PROVISIONAL  channels differ by 21%, and in the opposite direction
 *                       to r8 — CH2 reads HIGHER here and lower there, which
 *                       is not a fixed per-channel gain error and is the
 *                       clearest sign the sweep amplitudes were wrong for
 *                       these rows rather than the hardware being odd.
 */
static const scope_cal_tier_t tier_by_range[SCOPE_CAL_RANGE_COUNT] = {
    SCOPE_CAL_NONE,         /* 0 */
    SCOPE_CAL_NONE,         /* 1 */
    SCOPE_CAL_NONE,         /* 2 */
    SCOPE_CAL_NONE,         /* 3 */
    SCOPE_CAL_PROVISIONAL,  /* 4 */
    SCOPE_CAL_MEASURED,     /* 5 */
    SCOPE_CAL_MEASURED,     /* 6 */
    SCOPE_CAL_MEASURED,     /* 7 */
    SCOPE_CAL_PROVISIONAL,  /* 8 */
    SCOPE_CAL_PROVISIONAL,  /* 9 */
};

/* Channel 1/2 -> row 0/1, or -1 for anything else. Out-of-domain arguments
 * return "no calibration" rather than clamping into a neighbouring channel:
 * silently addressing the wrong channel is exactly how `fpga scope range
 * <n> 0` came to address both of them. */
static int channel_row(uint8_t ch)
{
    if (ch == 1u) return 0;
    if (ch == 2u) return 1;
    return -1;
}

float scope_cal_mv_per_count(uint8_t ch, uint8_t range_idx)
{
    const int row = channel_row(ch);

    if (row < 0 || range_idx >= SCOPE_CAL_RANGE_COUNT)
        return 0.0f;

    return mv_per_count[row][range_idx] * SCOPE_CAL_SOURCE_SCALE;
}

static scope_cal_probe_fn probe_src;

void scope_cal_set_probe_source(scope_cal_probe_fn fn)
{
    probe_src = fn;
}

float scope_cal_probe_factor(uint8_t ch)
{
    if (probe_src == NULL || channel_row(ch) < 0)
        return 1.0f;
    const float f = probe_src(ch);
    return (f > 0.0f) ? f : 1.0f;
}

float scope_cal_volts_per_count(uint8_t ch, uint8_t range_idx)
{
    return scope_cal_mv_per_count(ch, range_idx) / 1000.0f * scope_cal_probe_factor(ch);
}

scope_cal_tier_t scope_cal_get_tier(uint8_t ch, uint8_t range_idx)
{
    if (channel_row(ch) < 0 || range_idx >= SCOPE_CAL_RANGE_COUNT)
        return SCOPE_CAL_NONE;

    /* An entry with no gain cannot be anything but NONE, whatever the tier
     * table says. Keeping the two in sync is a maintenance hazard, so derive
     * the guard rather than trusting agreement. */
    if (mv_per_count[channel_row(ch)][range_idx] <= 0.0f)
        return SCOPE_CAL_NONE;

    return tier_by_range[range_idx];
}

float scope_cal_volts_per_div(uint8_t ch, uint8_t range_idx)
{
    return scope_cal_volts_per_count(ch, range_idx) * SCOPE_CAL_COUNTS_PER_DIV;
}

int scope_cal_true_scale_ok(uint8_t ch, uint8_t range_idx)
{
    /* Same gate the labels and badges use: a range has a volts meaning exactly
     * when its gain is > 0. Deriving it from the gain (not the tier table)
     * keeps the two from drifting apart, for the same reason
     * scope_cal_get_tier() does. */
    return scope_cal_volts_per_count(ch, range_idx) > 0.0f;
}

static void format_range_label(uint8_t ch, uint8_t range_idx, float v,
                               char *out, uint32_t n)
{
    if (out == NULL || n == 0u)
        return;

    if (v <= 0.0f) {
        /* No cal. The grid has no volts meaning on this range, and saying so
         * is the point — a nominal label here would be the defect this
         * module exists to remove. */
        snprintf(out, n, "--");
        return;
    }

    /* Provisional entries carry a leading '~' so the face of the instrument
     * distinguishes "measured" from "roughly". It costs one character and it
     * is the only place a user can see the difference. */
    const char *mark =
        (scope_cal_get_tier(ch, range_idx) == SCOPE_CAL_PROVISIONAL) ? "~" : "";

    if (v < 1.0f) {
        snprintf(out, n, "%s%umV", mark, (unsigned)(v * 1000.0f + 0.5f));
    } else {
        const unsigned centivolts = (unsigned)(v * 100.0f + 0.5f);
        snprintf(out, n, "%s%u.%02uV", mark, centivolts / 100u, centivolts % 100u);
    }
}

void scope_cal_range_label(uint8_t ch, uint8_t range_idx, char *out, uint32_t n)
{
    format_range_label(ch, range_idx, scope_cal_volts_per_div(ch, range_idx), out, n);
}

void scope_cal_range_label_bnc(uint8_t ch, uint8_t range_idx, char *out, uint32_t n)
{
    format_range_label(ch, range_idx,
                       scope_cal_mv_per_count(ch, range_idx) / 1000.0f * SCOPE_CAL_COUNTS_PER_DIV,
                       out, n);
}
