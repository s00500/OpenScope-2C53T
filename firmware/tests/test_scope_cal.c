/*
 * Host tests for the scope vertical calibration table.
 *
 * These are not numeric-accuracy tests — the numbers came off a bench and no
 * unit test can second-guess them. What is tested is the set of PROPERTIES
 * that stop this table failing the way its predecessor failed:
 *
 *   1. An uncalibrated range returns exactly 0.0f, so a caller that forgets
 *      to check gets an obviously-zero volt reading rather than a plausible
 *      small one.
 *   2. Out-of-domain channel/range arguments return "no calibration" instead
 *      of clamping into a neighbour.
 *   3. The source-scale factor reaches every entry uniformly, which is the
 *      property that makes one calibrated measurement able to rescale the
 *      whole instrument later.
 *   4. The MEASURED ranges really do agree between channels and really do
 *      form the doubling ladder claimed in the header, so if someone edits a
 *      row to "fix" a reading, the claim in the comment stops being true and
 *      the build says so.
 *   5. Labels are derived from the gain rather than hardcoded, and mark
 *      provisional entries.
 */

#include "../src/ui/scope_cal.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

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

static void test_uncalibrated_ranges_return_zero(void)
{
    /* Since EXP-92 every range has a gain on both channels (0-1 were NONE
     * until then, 0-3 before EXP-89). The NONE contract stays, and is what
     * a range without a gain gets: exactly 0.0f, tier NONE, no volts/div. */
    for (uint8_t ch = 1; ch <= 2; ch++) {
        for (uint8_t r = 0; r < SCOPE_CAL_RANGE_COUNT; r++)
            CHECK(scope_cal_mv_per_count(ch, r) > 0.0f, "ch%u r%u has no gain", ch, r);
        const uint8_t r = SCOPE_CAL_RANGE_COUNT;   /* no such range */
        CHECK(scope_cal_mv_per_count(ch, r) == 0.0f,
              "ch%u r%u: a range without a gain must return exactly 0.0f", ch, r);
        CHECK(scope_cal_get_tier(ch, r) == SCOPE_CAL_NONE,
              "ch%u r%u: a range without a gain must be tier NONE", ch, r);
        CHECK(scope_cal_volts_per_div(ch, r) == 0.0f,
              "ch%u r%u: a range without a gain must have no volts/div", ch, r);
    }
}

static void test_out_of_domain_arguments(void)
{
    /* Channel 0 is the trap: fpga_scope_set_range_diag_ch() uses 0-based
     * channels while the shell argument is 1-based, and that mismatch has
     * already caused one silently-wrong bench result. Here 0 must be a
     * refusal, not "CH1". */
    CHECK(scope_cal_mv_per_count(0, 6) == 0.0f, "ch0 must not resolve to CH1");
    CHECK(scope_cal_mv_per_count(3, 6) == 0.0f, "ch3 must not resolve");
    CHECK(scope_cal_mv_per_count(1, SCOPE_CAL_RANGE_COUNT) == 0.0f,
          "range past the end must not resolve");
    CHECK(scope_cal_mv_per_count(1, 255) == 0.0f, "range 255 must not resolve");
    CHECK(scope_cal_get_tier(0, 6) == SCOPE_CAL_NONE, "ch0 tier must be NONE");
}

static void test_source_scale_is_uniform(void)
{
    /*
     * Every calibrated entry must be the raw bench number times exactly one
     * shared factor. The test states the raw numbers independently of the
     * table, so if someone hand-edits a single row to match a reference
     * instrument, this fails — which is the intent. The recovery path when a
     * trusted source arrives is to change SCOPE_CAL_SOURCE_SCALE, and that
     * only works if no row has been individually nudged.
     */
    /* EXP-83/84 (2026-11-26, my unit). Unit #1's EXP-08 rows (14.08 / 21.83 /
     * 42.95 / 88.42 / 279.05 / 352.17 and 9.49 / 20.96 / 41.71 / 83.79 /
     * 223.71 / 425.00, x0.92) are withdrawn; see scope_cal.c. */
    /* r0-r2 from EXP-92 (r2 was 1.983 / 1.974 on 3-4 levels, withdrawn),
     * r3-r4 from EXP-89 (r4 was 7.85 / 7.82 on two levels, withdrawn). */
    static const float raw_ch1[] = { 0.4028f, 0.7981f, 1.9872f, 3.927f, 7.866f, 20.01f, 39.79f, 78.90f, 194.87f, 404.21f };
    static const float raw_ch2[] = { 0.3978f, 0.7928f, 1.9732f, 3.906f, 7.783f, 19.86f, 39.39f, 77.11f, 206.53f, 430.48f };

    for (uint8_t i = 0; i < 10; i++) {
        const uint8_t r = i;

        const float got1 = scope_cal_mv_per_count(1, r);
        const float got2 = scope_cal_mv_per_count(2, r);
        const float want1 = raw_ch1[i] * SCOPE_CAL_SOURCE_SCALE;
        const float want2 = raw_ch2[i] * SCOPE_CAL_SOURCE_SCALE;

        CHECK(fabsf(got1 - want1) < 0.01f,
              "ch1 r%u: expected %f (raw * source scale), got %f",
              r, (double)want1, (double)got1);
        CHECK(fabsf(got2 - want2) < 0.01f,
              "ch2 r%u: expected %f (raw * source scale), got %f",
              r, (double)want2, (double)got2);
    }
}

static void test_measured_tier_claims_hold(void)
{
    /*
     * The header claims ranges 5/6/7 agree between channels within 6% and
     * form a doubling ladder. Those are the grounds for calling them
     * MEASURED, so they are asserted rather than asserted-in-a-comment.
     */
    for (uint8_t r = 0; r <= 7; r++) {      /* 3/4 since EXP-89, 0-2 EXP-92 */
        CHECK(scope_cal_get_tier(1, r) == SCOPE_CAL_MEASURED,
              "r%u should be MEASURED on ch1", r);
        CHECK(scope_cal_get_tier(2, r) == SCOPE_CAL_MEASURED,
              "r%u should be MEASURED on ch2", r);

        const float a = scope_cal_mv_per_count(1, r);
        const float b = scope_cal_mv_per_count(2, r);
        const float disagreement = fabsf(a - b) / ((a + b) * 0.5f);

        CHECK(disagreement < 0.06f,
              "r%u: channels disagree by %.1f%%, too much for tier MEASURED",
              r, (double)(disagreement * 100.0f));
    }

    /* EXP-83/84/89/92: the whole ladder, r0->r1 2x, r1->r2 2.5x, r2->r3 2x,
     * r3->r4 2x, r4->r5 2.5x, r5->r6 2x, r6->r7 2x, r7->r8 2.5x, r8->r9 2x,
     * on both channels within 8 % (r8/r9 rest on 7-15 counts of span). */
    {
        const float step[] = { 2.0f, 2.5f, 2.0f, 2.0f, 2.5f, 2.0f, 2.0f, 2.5f, 2.0f };
        for (uint8_t ch = 1; ch <= 2; ch++)
            for (uint8_t r = 0; r <= 8; r++) {
                const float got = scope_cal_mv_per_count(ch, (uint8_t)(r + 1u)) /
                                  scope_cal_mv_per_count(ch, r);
                CHECK(fabsf(got / step[r] - 1.0f) < 0.08f,
                      "ch%u r%u->r%u: step %.2f, expected ~%.1f", ch, r, r + 1,
                      (double)got, (double)step[r]);
            }
    }

    /* Doubling ladder: each step up should roughly double the mV/count. */
    for (uint8_t ch = 1; ch <= 2; ch++) {
        for (uint8_t r = 5; r <= 6; r++) {
            const float ratio = scope_cal_mv_per_count(ch, (uint8_t)(r + 1u)) /
                                scope_cal_mv_per_count(ch, r);
            CHECK(ratio > 1.8f && ratio < 2.2f,
                  "ch%u r%u->r%u: ratio %.2f is not the claimed doubling",
                  ch, r, r + 1, (double)ratio);
        }
    }
}

static void test_provisional_ranges_still_produce_a_number(void)
{
    /* Provisional is not "unusable": a rough volt figure beats a raw count,
     * and the tier plus the label marker is how the user is told which is
     * which. */
    const uint8_t provisional[] = { 8, 9 };   /* r4 MEASURED since EXP-89, r2 EXP-92 */

    for (uint8_t i = 0; i < 2; i++) {
        const uint8_t r = provisional[i];
        for (uint8_t ch = 1; ch <= 2; ch++) {
            CHECK(scope_cal_mv_per_count(ch, r) > 0.0f,
                  "ch%u r%u: provisional range must still return a gain", ch, r);
            CHECK(scope_cal_get_tier(ch, r) == SCOPE_CAL_PROVISIONAL,
                  "ch%u r%u: expected tier PROVISIONAL", ch, r);
        }
    }
}

static void test_volts_per_div_is_derived(void)
{
    for (uint8_t ch = 1; ch <= 2; ch++) {
        for (uint8_t r = 0; r < SCOPE_CAL_RANGE_COUNT; r++) {
            const float want = scope_cal_volts_per_count(ch, r) *
                               SCOPE_CAL_COUNTS_PER_DIV;
            CHECK(fabsf(scope_cal_volts_per_div(ch, r) - want) < 1e-6f,
                  "ch%u r%u: volts/div must be gain * counts-per-div", ch, r);
        }
    }
}

static void test_labels(void)
{
    char buf[16];

    /* Uncalibrated -> an explicit nothing, never a nominal number. */
    scope_cal_range_label(3, 1, buf, sizeof(buf));
    CHECK(strcmp(buf, "--") == 0, "no calibration: label should be \"--\", got \"%s\"", buf);

    /* The sensitive rows: 3.927 * 32 = 125.7 mV (EXP-89), 1.9872 * 32 =
     * 63.6 and 0.4028 * 32 = 12.9 mV (EXP-92), all measured, no tilde. */
    scope_cal_range_label(1, 3, buf, sizeof(buf));
    CHECK(strcmp(buf, "126mV") == 0, "r3 label should be 126mV, got \"%s\"", buf);
    scope_cal_range_label(1, 2, buf, sizeof(buf));
    CHECK(strcmp(buf, "64mV") == 0, "r2 label should be 64mV, got \"%s\"", buf);
    scope_cal_range_label(1, 0, buf, sizeof(buf));
    CHECK(strcmp(buf, "13mV") == 0, "r0 label should be 13mV, got \"%s\"", buf);

    /* Measured -> a plain number, no marker. */
    scope_cal_range_label(1, 6, buf, sizeof(buf));
    CHECK(buf[0] != '~', "r6 is MEASURED and must not be marked, got \"%s\"", buf);
    CHECK(strchr(buf, 'V') != NULL, "r6 label should carry units, got \"%s\"", buf);

    /* 39.79 mV/count * 1.0 source-scale * 32 counts/div = 1.2733 V/div. */
    CHECK(strcmp(buf, "1.27V") == 0,
          "r6 label should be derived as 1.27V, got \"%s\"", buf);

    /* Provisional -> same number, marked. */
    scope_cal_range_label(1, 9, buf, sizeof(buf));
    CHECK(buf[0] == '~', "r9 is PROVISIONAL and must be marked, got \"%s\"", buf);

    /* Sub-volt divisions read in mV. 20.01 * 1.0 * 32 = 640.3 mV. */
    scope_cal_range_label(1, 5, buf, sizeof(buf));
    CHECK(strcmp(buf, "640mV") == 0,
          "r5 label should be derived as 640mV, got \"%s\"", buf);

    /*
     * And the point of the whole exercise: the derived label must NOT equal
     * the nominal vdiv_table label it replaces. r5's nominal label was
     * "200mV" and r6's was "500mV", both roughly 3x out.
     */
    scope_cal_range_label(1, 5, buf, sizeof(buf));
    CHECK(strcmp(buf, "200mV") != 0, "r5 must not still read as the nominal 200mV");
    scope_cal_range_label(1, 6, buf, sizeof(buf));
    CHECK(strcmp(buf, "500mV") != 0, "r6 must not still read as the nominal 500mV");

    /* A tiny buffer must truncate safely, not overrun. */
    char small[3];
    scope_cal_range_label(1, 9, small, sizeof(small));
    CHECK(small[2] == '\0', "label must NUL-terminate within a 3-byte buffer");
}

/* The true-scale render gate (M3) must agree, range for range, with whether a
 * volts/div exists: calibrated -> render true scale; NONE / out-of-domain ->
 * autofit. If these two ever disagree, the grid would claim a volts/div the
 * badges refuse to print (or vice versa) — the exact class of inconsistency
 * scope_cal exists to prevent. */
static void test_true_scale_gate_matches_calibration(void)
{
    for (uint8_t ch = 1; ch <= 2; ch++) {
        for (uint8_t r = 0; r < SCOPE_CAL_RANGE_COUNT; r++) {
            int ok   = scope_cal_true_scale_ok(ch, r);
            int cal  = scope_cal_volts_per_count(ch, r) > 0.0f;
            CHECK(!!ok == cal,
                  "ch%u r%u: true_scale_ok(%d) must match has-cal(%d)",
                  ch, r, ok, cal);
        }
        /* No gain -> must autofit (only a missing range now); 0-7 are
         * MEASURED -> true scale. */
        CHECK(!scope_cal_true_scale_ok(ch, SCOPE_CAL_RANGE_COUNT),
              "ch%u: a range without a gain must NOT render true scale", ch);
        for (uint8_t r = 0; r <= 7; r++)
            CHECK(scope_cal_true_scale_ok(ch, r),
                  "ch%u r%u: measured range must render true scale", ch, r);
    }
    /* Out-of-domain is a refusal, same as everywhere else in this module. */
    CHECK(!scope_cal_true_scale_ok(0, 6), "ch0 must not render true scale");
    CHECK(!scope_cal_true_scale_ok(1, SCOPE_CAL_RANGE_COUNT),
          "range past the end must not render true scale");
}

/* Probe factor (2026-10-03): the readout scales, the calibration table does
 * not, and only the channel with the 10x probe moves. Control: with no
 * source registered every number is exactly what it was. */
static float probe_ch2_x10(uint8_t ch) { return ch == 2u ? 10.0f : 1.0f; }
static float probe_bad(uint8_t ch)     { (void)ch; return -3.0f; }

static void test_probe_factor(void)
{
    const uint8_t r = 6;                       /* a MEASURED range */
    scope_cal_set_probe_source(NULL);
    const float v1 = scope_cal_volts_per_count(1, r), v2 = scope_cal_volts_per_count(2, r);
    const float m2 = scope_cal_mv_per_count(2, r), d2 = scope_cal_volts_per_div(2, r);
    char l_before[12], l_after[12];
    scope_cal_range_label(2, r, l_before, sizeof(l_before));
    CHECK(v2 > 0.0f && scope_cal_probe_factor(2) == 1.0f, "no source: factor 1");

    scope_cal_set_probe_source(probe_ch2_x10);
    CHECK(scope_cal_volts_per_count(2, r) == v2 * 10.0f, "CH2 10x: volts/count x10");
    CHECK(scope_cal_volts_per_div(2, r) == d2 * 10.0f, "CH2 10x: volts/div x10");
    CHECK(scope_cal_mv_per_count(2, r) == m2, "CH2 10x: the cal table must not move");
    CHECK(scope_cal_volts_per_count(1, r) == v1, "CH1 1x: unchanged");
    scope_cal_range_label(2, r, l_after, sizeof(l_after));
    CHECK(strcmp(l_before, l_after) != 0, "CH2 label must change (%s -> %s)", l_before, l_after);
    {
        char bnc[12];
        scope_cal_range_label_bnc(2, r, bnc, sizeof(bnc));
        CHECK(strcmp(bnc, l_before) == 0, "BNC label ignores the probe (%s vs %s)", bnc, l_before);
    }
    CHECK(scope_cal_volts_per_count(2, SCOPE_CAL_RANGE_COUNT) == 0.0f,
          "a range without a gain stays 0 at 10x");
    CHECK(scope_cal_true_scale_ok(2, r), "true-scale gate unaffected by the probe");

    scope_cal_set_probe_source(probe_bad);
    CHECK(scope_cal_volts_per_count(2, r) == v2, "a nonsense factor falls back to 1");
    scope_cal_set_probe_source(NULL);
    CHECK(scope_cal_volts_per_count(2, r) == v2, "unregistering restores 1x");
}

static void test_zero_model(void)
{
    /*
     * EXP-89 (2026-11-26, my unit), input shorted, DC: every measured point,
     * stated here independently of the table. The model (one zero per range
     * at code 2048, one counts-per-code per channel) must reproduce all 50
     * of them within 0.3 counts. CH2's third code tests linearity; CH1's
     * second tests that one slope serves every range.
     */
    static const float ch1_2048[] = { 48.80f, 45.96f, 44.39f, 44.06f, 43.73f, 48.85f, 45.98f, 44.67f, 44.09f, 43.79f };
    static const float ch1_2560[] = { 110.92f, 108.17f, 106.32f, 105.86f, 105.54f, 110.96f, 108.21f, 106.80f, 105.87f, 105.57f };
    static const float ch2_2048[] = { 40.56f, 42.70f, 43.68f, 44.34f, 44.64f, 40.62f, 42.91f, 43.82f, 44.77f, 44.90f };
    static const float ch2_2544[] = { 101.09f, 102.95f, 104.20f, 104.75f, 105.14f, 101.20f, 103.16f, 104.39f, 105.42f, 105.63f };
    static const float ch2_3040[] = { 161.89f, 163.44f, 164.83f, 165.04f, 165.21f, 162.03f, 163.87f, 165.05f, 165.57f, 165.71f };
    struct { uint8_t ch; uint16_t code; const float *want; } pts[] = {
        { 1, 2048, ch1_2048 }, { 1, 2560, ch1_2560 },
        { 2, 2048, ch2_2048 }, { 2, 2544, ch2_2544 }, { 2, 3040, ch2_3040 },
    };
    for (unsigned i = 0; i < sizeof pts / sizeof pts[0]; i++)
        for (uint8_t r = 0; r < SCOPE_CAL_RANGE_COUNT; r++) {
            float z = -1.0f;
            CHECK(scope_cal_zero_counts(pts[i].ch, r, pts[i].code, &z), "ch%u r%u zero refused", pts[i].ch, r);
            CHECK(fabsf(z - pts[i].want[r]) < 0.3f, "ch%u r%u code %u: model %.2f, measured %.2f",
                  pts[i].ch, r, pts[i].code, (double)z, (double)pts[i].want[r]);
        }

    /* dac1_linearity.py: GEN held at its lowest level, so only differences
     * from code 2048 are the zero's; two points each within 0.29 of the
     * model allow 0.5 for their difference. */
    {
        static const uint16_t codes[] = { 2048, 2304, 2560, 2816, 3072 };
        static const float r5[] = { 49.91f, 81.19f, 112.38f, 143.43f, 174.61f };
        static const float r9[] = { 43.86f, 74.49f, 105.66f, 137.03f, 168.25f };
        for (unsigned i = 1; i < 5; i++) {
            float a, b, c, d;
            scope_cal_zero_counts(1, 5, codes[i], &a); scope_cal_zero_counts(1, 5, 2048, &b);
            scope_cal_zero_counts(1, 9, codes[i], &c); scope_cal_zero_counts(1, 9, 2048, &d);
            CHECK(fabsf((a - b) - (r5[i] - r5[0])) < 0.5f, "ch1 r5 2048->%u: model %+.2f, measured %+.2f",
                  codes[i], (double)(a - b), (double)(r5[i] - r5[0]));
            CHECK(fabsf((c - d) - (r9[i] - r9[0])) < 0.5f, "ch1 r9 2048->%u: model %+.2f, measured %+.2f",
                  codes[i], (double)(c - d), (double)(r9[i] - r9[0]));
        }
    }

    /* Volts: 0 V at the zero, one division above it is volts/div. */
    for (uint8_t ch = 1; ch <= 2; ch++)
        for (uint8_t r = 0; r <= 9; r++) {
            float z, v0 = 1.0f, v1 = 0.0f;
            scope_cal_zero_counts(ch, r, 2300, &z);
            CHECK(scope_cal_counts_to_volts(ch, r, 2300, z, &v0) && fabsf(v0) < 1e-6f,
                  "ch%u r%u: the zero is not 0 V (%f)", ch, r, (double)v0);
            CHECK(scope_cal_counts_to_volts(ch, r, 2300, z + SCOPE_CAL_COUNTS_PER_DIV, &v1) &&
                  fabsf(v1 - scope_cal_volts_per_div(ch, r)) < 1e-4f * scope_cal_volts_per_div(ch, r),
                  "ch%u r%u: a division above 0 V is %f, volts/div %f", ch, r, (double)v1,
                  (double)scope_cal_volts_per_div(ch, r));
        }

    /* A zero without a gain is not a volt; nonsense arguments are refused. */
    {
        float v = 123.0f, z = 123.0f;
        CHECK(!scope_cal_counts_to_volts(1, SCOPE_CAL_RANGE_COUNT, 2048, 100.0f, &v) && v == 123.0f,
              "a range without a gain gave a volt");
        CHECK(!scope_cal_zero_counts(0, 5, 2048, &z) && z == 123.0f, "ch0 must not resolve");
        CHECK(!scope_cal_zero_counts(3, 5, 2048, &z), "ch3 must not resolve");
        CHECK(!scope_cal_zero_counts(1, SCOPE_CAL_RANGE_COUNT, 2048, &z), "range past the end");
        CHECK(!scope_cal_counts_to_volts(1, 5, 2048, 100.0f, NULL), "NULL out");
    }

    /* The inverse lands back on the code, and clamps. */
    for (uint8_t ch = 1; ch <= 2; ch++)
        for (uint16_t code = 1600; code <= 3600; code += 250) {
            float z;
            scope_cal_zero_counts(ch, 5, code, &z);
            const uint16_t back = scope_cal_offset_for_zero(ch, 5, z);
            CHECK(back == code, "ch%u: inverse of code %u gave %u", ch, code, back);
        }
    CHECK(scope_cal_offset_for_zero(1, 5, -500.0f) == 0u, "clamp low");
    CHECK(scope_cal_offset_for_zero(2, 5, 900.0f) == 4095u, "clamp high");
    CHECK(scope_cal_offset_for_zero(0, 5, 128.0f) == SCOPE_CAL_ZERO_REF_CODE, "bad ch -> ref");
    /* 0 V mid-scale on CH1 r5 needs about 2700 (EXP-89 arithmetic). */
    CHECK(abs((int)scope_cal_offset_for_zero(1, 5, 128.0f) - 2701) <= 2, "CH1 r5 centre code %u",
          scope_cal_offset_for_zero(1, 5, 128.0f));
}

static void test_format_volts(void)
{
    static const struct { float v; const char *want; } t[] = {
        { 0.0f, "0mV" }, { 0.0004f, "0mV" }, { -0.0004f, "0mV" },
        { 0.0449f, "+45mV" }, { -0.045f, "-45mV" }, { 0.9994f, "+999mV" },
        { 0.9996f, "+1.00V" }, { 1.234f, "+1.23V" }, { -2.5f, "-2.50V" },
        { 9.994f, "+9.99V" }, { 9.996f, "+10.0V" }, { 12.34f, "+12.3V" },
        { -99.96f, "-100V" }, { 123.4f, "+123V" },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        char b[12];
        scope_cal_format_volts(t[i].v, b, sizeof b);
        CHECK(strcmp(b, t[i].want) == 0, "format %f: got \"%s\", want \"%s\"", (double)t[i].v, b, t[i].want);
    }
    char small[4];
    scope_cal_format_volts(1.234f, small, sizeof small);
    CHECK(strlen(small) == 3, "truncates to the buffer");
}

int main(void)
{
    test_uncalibrated_ranges_return_zero();
    test_out_of_domain_arguments();
    test_true_scale_gate_matches_calibration();
    test_source_scale_is_uniform();
    test_measured_tier_claims_hold();
    test_provisional_ranges_still_produce_a_number();
    test_volts_per_div_is_derived();
    test_labels();
    test_probe_factor();
    test_zero_model();
    test_format_volts();

    if (failures == 0) {
        printf("test_scope_cal: all checks passed\n");
        return 0;
    }
    printf("test_scope_cal: %d check(s) FAILED\n", failures);
    return 1;
}
