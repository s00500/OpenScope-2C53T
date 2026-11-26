/*
 * Host tests for the scope horizontal (time) calibration.
 *
 * As with test_scope_cal.c, these do not second-guess bench numbers. They test
 * the PROPERTIES that keep this table from failing the way rate figures have
 * already failed on this project:
 *
 *   1. A code with no trustworthy rate returns exactly 0.0f, so a caller that
 *      forgets to check gets an obviously-zero frequency rather than a
 *      plausible one.
 *   2. Code 0x08 had three published rates, all artifacts; it now has a
 *      measured one (EXP-81) and the artifacts must not come back. The same
 *      holds for every superseded ladder.
 *   3. Out-of-range codes refuse rather than wrap.
 *   4. Derived quantities really are derived — s/div from the rate and the
 *      renderer's samples-per-division, Hz from the rate and a period.
 *   5. Labels come from the measurement and are NOT the nominal timebase_table
 *      strings, which are a constant ~2.13x away.
 */

#include "../src/ui/scope_timebase.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

static int failures = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static void test_measured_codes(void)
{
    /*
     * EXP-81 (2026-11-26, my unit): every code 0x06-0x14 against the onboard
     * generator, converted to seconds with the Rigol's +54.2 ppm. The rows
     * must hold those numbers, so a merge that brings back an older ladder
     * fails here.
     */
    const struct { uint8_t code; float fs; scope_tb_tier_t tier; } want[] = {
        { 0x06, 25001207.8f, SCOPE_TB_PROVISIONAL },
        { 0x07, 12500078.1f, SCOPE_TB_PROVISIONAL },
        { 0x08,  5000009.5f, SCOPE_TB_MEASURED },
        { 0x09,  2500024.7f, SCOPE_TB_MEASURED },
        { 0x0A,  1250006.2f, SCOPE_TB_MEASURED },
        { 0x0B,   499999.1f, SCOPE_TB_MEASURED },
        { 0x0C,   250000.8f, SCOPE_TB_MEASURED },
        { 0x0D,   125000.1f, SCOPE_TB_MEASURED },
        { 0x0E,    50000.0f, SCOPE_TB_MEASURED },
        { 0x0F,    25000.0f, SCOPE_TB_MEASURED },
        { 0x10,    12500.0f, SCOPE_TB_MEASURED },
        { 0x11,     5000.0f, SCOPE_TB_MEASURED },
        { 0x12,     2500.0f, SCOPE_TB_MEASURED },
        { 0x13,     1250.0f, SCOPE_TB_MEASURED },
        { 0x14,      500.0f, SCOPE_TB_MEASURED },
    };
    const unsigned n = sizeof(want) / sizeof(want[0]);

    for (unsigned i = 0; i < n; i++) {
        const float got = scope_timebase_sample_rate(want[i].code);
        CHECK(fabsf(got / want[i].fs - 1.0f) < 1e-6f,
              "code 0x%02X: expected %.1f S/s (EXP-81), got %.1f",
              want[i].code, (double)want[i].fs, (double)got);
        CHECK(scope_timebase_get_tier(want[i].code) == want[i].tier,
              "code 0x%02X: wrong tier", want[i].code);
    }

    /*
     * The finding: each code is the round 1-2.5-5 ladder within 60 ppm in
     * seconds. Asserted, not just commented, so a transcription slip or a
     * stray older value fails the build.
     */
    const float ladder[] = { 25e6f, 12.5e6f, 5e6f, 2.5e6f, 1.25e6f, 500e3f, 250e3f,
                             125e3f, 50e3f, 25e3f, 12.5e3f, 5e3f, 2.5e3f, 1250.0f, 500.0f };
    for (unsigned i = 0; i < n; i++) {
        const float got = scope_timebase_sample_rate((uint8_t)(0x06 + i));
        CHECK(fabsf(got / ladder[i] - 1.0f) < 60e-6f,
              "code 0x%02X is %.1f, more than 60 ppm from the ladder %.0f",
              0x06 + i, (double)got, (double)ladder[i]);
    }

    /*
     * The ladder is 1-2.5-5, NOT uniform x2 -- predicting 6,250 for 0x11 by
     * assuming each step doubles was wrong by 25% (EXP-18). Encode the real
     * cadence over every code that has a rate.
     */
    for (uint8_t c = 0x06; c < 0x14; c++) {
        const float got = scope_timebase_sample_rate(c) / scope_timebase_sample_rate((uint8_t)(c + 1));
        const float step = ((0x14 - c) % 3 == 1) ? 2.5f : 2.0f;
        CHECK(fabsf(got - step) < 0.01f,
              "0x%02X/0x%02X should step by %.1fx, got %.4fx", c, c + 1, (double)step, (double)got);
    }
}

static void test_code_08_artifacts_stay_out(void)
{
    /*
     * 0x08 had three published rates -- 1.07 kS/s, 1,660 S/s, and a two-pass
     * 1,414/1,526 -- and every one was fitted to AUTO records whose two
     * channels came from different moments (#84). It now has a measured rate
     * (EXP-81, 5.000 MS/s, the power-on code); the artifacts must not come
     * back on a merge.
     */
    const float bad[] = { 1070.0f, 1660.0f, 1414.0f, 1526.0f };
    for (unsigned i = 0; i < 4; i++)
        CHECK(fabsf(scope_timebase_sample_rate(0x08) - bad[i]) > 50.0f,
              "code 0x08 is back at a withdrawn artifact (%.0f S/s)", (double)bad[i]);
    CHECK(scope_timebase_get_tier(0x08) == SCOPE_TB_MEASURED, "code 0x08 is MEASURED");
}

static void test_superseded_rates_stay_out(void)
{
    /*
     * Every superseded ladder stays out:
     *   EXP-14  the first ladder, 1.21x too high (a source scale error);
     *   EXP-17  the second, 0.7-2.9% off per code;
     *   EXP-17/18 unit #1's rows, 0.07-0.20% low, 0x0D 1.06% low (EXP-81).
     */
    const struct { uint8_t code; float bad; } gone[] = {
        { 0x0D, 150706.0f }, { 0x0E, 62958.0f }, { 0x0F, 30235.0f }, { 0x10, 14853.6f },
        { 0x0E, 49056.0f }, { 0x0F, 25736.0f }, { 0x10, 12575.0f },
        { 0x0D, 123662.7f }, { 0x0E, 49930.1f }, { 0x0F, 24979.1f }, { 0x10, 12490.0f },
        { 0x11, 4990.8f }, { 0x12, 2494.9f },
    };
    for (unsigned i = 0; i < sizeof(gone) / sizeof(gone[0]); i++) {
        CHECK(fabsf(scope_timebase_sample_rate(gone[i].code) - gone[i].bad) > 0.5f,
              "code 0x%02X is back at a superseded rate %.1f", gone[i].code,
              (double)gone[i].bad);
    }
}

static void test_unmeasured_codes_return_zero(void)
{
    for (uint8_t c = 0; c <= 0x05; c++) {
        CHECK(scope_timebase_sample_rate(c) == 0.0f,
              "code 0x%02X has no measured rate and must return 0.0f", c);
        CHECK(scope_timebase_get_tier(c) == SCOPE_TB_NONE,
              "code 0x%02X must be tier NONE", c);
        CHECK(scope_timebase_hz_from_period(c, 50.0f) == 0.0f,
              "code 0x%02X must not yield a frequency", c);
    }
}

static void test_out_of_range(void)
{
    CHECK(scope_timebase_sample_rate(SCOPE_TIMEBASE_CODE_COUNT) == 0.0f,
          "a code past the end must not resolve");
    CHECK(scope_timebase_sample_rate(255) == 0.0f, "code 255 must not resolve");
    CHECK(scope_timebase_get_tier(200) == SCOPE_TB_NONE,
          "an out-of-range tier must be NONE");
}

static void test_derived_quantities(void)
{
    for (uint8_t c = 0; c < SCOPE_TIMEBASE_CODE_COUNT; c++) {
        const float fs = scope_timebase_sample_rate(c);
        if (fs <= 0.0f) continue;

        CHECK(fabsf(scope_timebase_seconds_per_sample(c) - 1.0f / fs) < 1e-12f,
              "code 0x%02X: s/sample must be 1/fs", c);
        CHECK(fabsf(scope_timebase_seconds_per_div(c) -
                    (SCOPE_TIMEBASE_SAMPLES_PER_DIV / fs)) < 1e-12f,
              "code 0x%02X: s/div must be samples-per-div / fs", c);
        CHECK(fabsf(scope_timebase_hz_from_period(c, 100.0f) - fs / 100.0f) < 1e-6f,
              "code 0x%02X: Hz must be fs / period_samples", c);
    }

    /* A non-positive period is not a measurement. */
    CHECK(scope_timebase_hz_from_period(0x10, 0.0f) == 0.0f,
          "zero period must not yield a frequency");
    CHECK(scope_timebase_hz_from_period(0x10, -5.0f) == 0.0f,
          "negative period must not yield a frequency");
}

static void test_labels(void)
{
    char buf[16];

    /* 12500 S/s, 32 samples/div -> 2.56 ms/div. */
    scope_timebase_label(0x10, buf, sizeof(buf));
    CHECK(strcmp(buf, "2.56ms") == 0,
          "0x10 label should be derived as 2.56ms, got \"%s\"", buf);

    /* And it must NOT be the nominal table's "1ms" — that is the whole point.
     * The nominal labels are a constant ~2.13x away on every measured code. */
    CHECK(strcmp(buf, "1ms") != 0, "0x10 must not still read as the nominal 1ms");

    /* 25000 S/s -> 1.28 ms/div. */
    scope_timebase_label(0x0F, buf, sizeof(buf));
    CHECK(strcmp(buf, "1.28ms") == 0,
          "0x0F label should be 1.28ms, got \"%s\"", buf);

    /* 50000 S/s -> 640 us/div, sub-millisecond so microseconds. */
    scope_timebase_label(0x0E, buf, sizeof(buf));
    CHECK(strcmp(buf, "640us") == 0,
          "0x0E label should be 640us, got \"%s\"", buf);

    /* Below 10 us one decimal: 5 MS/s -> 6.4 us/div (the power-on code). */
    scope_timebase_label(0x08, buf, sizeof(buf));
    CHECK(strcmp(buf, "6.4us") == 0, "0x08 label should be 6.4us, got \"%s\"", buf);
    scope_timebase_label(0x09, buf, sizeof(buf));
    CHECK(strcmp(buf, "13us") == 0, "0x09 label should be 13us, got \"%s\"", buf);

    /* Provisional carries the marker: 0x06 rests on one tone at 1.8 cycles. */
    scope_timebase_label(0x06, buf, sizeof(buf));
    CHECK(strcmp(buf, "~1.3us") == 0, "0x06 is PROVISIONAL: expected ~1.3us, got \"%s\"", buf);

    /* No rate -> explicit nothing, never a nominal number. */
    scope_timebase_label(0x00, buf, sizeof(buf));
    CHECK(strcmp(buf, "--") == 0, "0x00 label should be \"--\", got \"%s\"", buf);
    scope_timebase_label(0x05, buf, sizeof(buf));
    CHECK(strcmp(buf, "--") == 0, "0x05 label should be \"--\", got \"%s\"", buf);

    /* Truncation must be safe. */
    char small[3];
    scope_timebase_label(0x10, small, sizeof(small));
    CHECK(small[2] == '\0', "label must NUL-terminate within a 3-byte buffer");
}

int main(void)
{
    test_measured_codes();
    test_code_08_artifacts_stay_out();
    test_superseded_rates_stay_out();
    test_unmeasured_codes_return_zero();
    test_out_of_range();
    test_derived_quantities();
    test_labels();

    if (failures == 0) {
        printf("test_scope_timebase: all checks passed\n");
        return 0;
    }
    printf("test_scope_timebase: %d check(s) FAILED\n", failures);
    return 1;
}
