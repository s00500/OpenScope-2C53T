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
 * Ranges 2-4 from EXP-89 (same day, same source and method), with the
 * offset reference at code 2048 so 0 V sat near 44 counts instead of
 * mid-scale: r2 3-4 levels, r3 7, r4 8 (EXP-83/84 got two below the rail on
 * r4). Ranges 0-1 need a finer step than GEN's 0.1 V and stay at 0.0f.
 * EXP-89 repeated r5-r9 too, within 0.0-0.5 % (CH1) and 0.3-2.0 % (CH2) of
 * the rows here, which are left as measured.
 * WITHDRAWN 2026-11-26 (EXP-89): r4 7.85 (CH1) / 7.82 (CH2), two levels each.
 *
 * Ranges 0-2 from EXP-92 (same night): the generator at raw DAC codes
 * (`siggen dccode`, 0.79 mV steps instead of amp's 98 mV), 7-9 levels per
 * range, fit SE 0.10-0.38 %, residuals <= 0.88 counts; every range puts the
 * generator's own 0 V at +8.7..+10.0 mV against the EXP-89 zero, on both
 * channels. Range 3 was re-measured there too (3.934 / 3.918, within 0.3 %
 * of the rows here, which are kept).
 * WITHDRAWN 2026-11-26 (EXP-92): r2 1.983 (CH1) / 1.974 (CH2), 3-4 levels.
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
    /*        r0       r1       r2       r3      r4      r5      r6      r7       r8       r9 */
    /* CH1 */ { 0.4028f, 0.7981f, 1.9872f, 3.927f, 7.866f, 20.01f, 39.79f, 78.90f, 194.87f, 404.21f },
    /* CH2 */ { 0.3978f, 0.7928f, 1.9732f, 3.906f, 7.783f, 19.86f, 39.39f, 77.11f, 206.53f, 430.48f },
};

/*
 * Confidence per range. Identical for both channels by construction: the
 * tier is a statement about how well the two channels AGREE, so it cannot
 * differ between them.
 *
 * 2026-11-26 (EXP-92, my unit):
 *   r0     MEASURED     channels within 1.3 %, 7 levels each, residuals
 *                       <= 0.39 counts (12.9 / 12.7 mV per division)
 *   r1     MEASURED     0.7 %, 8 levels each, residuals <= 0.67 counts
 *   r2     MEASURED     0.7 %, 9 levels each (before: PROVISIONAL, 3-4
 *                       levels at the generator's 0.1 V steps, EXP-89)
 * 2026-11-26 (EXP-89, my unit):
 *   r0-r1  NONE         not railed (the short reads 40-49 counts on every
 *                       range), but GEN's 0.1 V step is >100 counts here:
 *                       no gain measured (superseded by EXP-92)
 *   r2     PROVISIONAL  channels agree within 0.5 %, but 3-4 levels fit
 *                       below the rail (the range spans ~0.5 V)
 *   r3     MEASURED     0.5 %, 7 levels each, max residual 0.16-0.56 counts
 *   r4     MEASURED     1.1 %, 8 levels each, max residual 0.20-0.65 counts
 *                       (before: PROVISIONAL, two levels, EXP-83/84)
 * 2026-11-26 (EXP-83/84, my unit):
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
    SCOPE_CAL_MEASURED,     /* 0  EXP-92 */
    SCOPE_CAL_MEASURED,     /* 1  EXP-92 */
    SCOPE_CAL_MEASURED,     /* 2  EXP-92 */
    SCOPE_CAL_MEASURED,     /* 3 */
    SCOPE_CAL_MEASURED,     /* 4 */
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

/*
 * 0 V in ADC counts per (channel, range), offset reference at
 * SCOPE_CAL_ZERO_REF_CODE (2048), my unit, 2026-11-26.
 * Source: docs/experiments/2026-11-26-89-the-zero.md. Input shorted (probe
 * tip on its ground clip), DC coupling, 4 records (4,000 samples) per point,
 * frame-to-frame spread <= 0.08 counts; CH1 at DAC1 2048/2560 (1536 rails
 * low), CH2 at TMR13 2048/2544/3040.
 *
 * Fitted per range by least squares with one slope per channel (the zero
 * at code 2048 is the fit's intercept, not the 2048 reading). CH1's slope
 * also uses `dac1_linearity.py` (GEN held at its lowest level, DAC1
 * 2048..3072 on ranges 5 and 9, each run with its own intercept): its 30
 * points within 0.29 counts, rms 0.14; CH2's 30 within 0.27, rms 0.14.
 *
 * Ranges 0..4 repeat 5..9 within 0.3 counts on both channels, which says the
 * two halves share their five taps. (Ranges 0..3 had a zero but no gain
 * until EXP-89/92 measured them.)
 */
static const float zero_ref[2][SCOPE_CAL_RANGE_COUNT] = {
    /*        r0      r1      r2      r3      r4      r5      r6      r7      r8      r9 */
    /* CH1 */ { 48.79f, 46.00f, 44.29f, 43.89f, 43.56f, 48.83f, 46.02f, 44.66f, 43.91f, 43.61f },
    /* CH2 */ { 40.70f, 42.54f, 43.75f, 44.23f, 44.51f, 40.80f, 42.83f, 43.93f, 44.77f, 44.93f },
};

/* Counts per offset-reference code, the same on every range (sd 0.0003
 * across the ten). */
static const float zero_per_code[2] = { 0.12137f, 0.12195f };

bool scope_cal_zero_counts(uint8_t ch, uint8_t range_idx, uint16_t offset_code, float *out)
{
    const int row = channel_row(ch);
    if (row < 0 || range_idx >= SCOPE_CAL_RANGE_COUNT || out == NULL)
        return false;
    *out = zero_ref[row][range_idx] +
           zero_per_code[row] * ((float)offset_code - (float)SCOPE_CAL_ZERO_REF_CODE);
    return true;
}

bool scope_cal_counts_to_volts(uint8_t ch, uint8_t range_idx, uint16_t offset_code,
                               float counts, float *out)
{
    float z;
    const float vpc = scope_cal_volts_per_count(ch, range_idx);
    if (vpc <= 0.0f || out == NULL || !scope_cal_zero_counts(ch, range_idx, offset_code, &z))
        return false;
    *out = (counts - z) * vpc;
    return true;
}

uint16_t scope_cal_offset_for_zero(uint8_t ch, uint8_t range_idx, float counts)
{
    const int row = channel_row(ch);
    if (row < 0 || range_idx >= SCOPE_CAL_RANGE_COUNT)
        return (uint16_t)SCOPE_CAL_ZERO_REF_CODE;
    const float c = (float)SCOPE_CAL_ZERO_REF_CODE +
                    (counts - zero_ref[row][range_idx]) / zero_per_code[row];
    if (c <= 0.0f)    return 0u;
    if (c >= 4095.0f) return 4095u;
    return (uint16_t)(c + 0.5f);
}

void scope_cal_format_volts(float v, char *out, uint32_t n)
{
    if (out == NULL || n == 0u)
        return;
    float a = (v < 0.0f) ? -v : v;
    if (a > 99999.0f) a = 99999.0f;
    const unsigned mv = (unsigned)(a * 1000.0f + 0.5f);
    const char s = (mv == 0u) ? '\0' : ((v < 0.0f) ? '-' : '+');
    char sign[2] = { s, '\0' };

    if (mv < 1000u) {
        snprintf(out, n, "%s%umV", sign, mv);
    } else if (mv < 9995u) {
        const unsigned cv = (mv + 5u) / 10u;
        snprintf(out, n, "%s%u.%02uV", sign, cv / 100u, cv % 100u);
    } else if (mv < 99950u) {
        const unsigned dv = (mv + 50u) / 100u;
        snprintf(out, n, "%s%u.%uV", sign, dv / 10u, dv % 10u);
    } else {
        snprintf(out, n, "%s%uV", sign, (mv + 500u) / 1000u);
    }
}
