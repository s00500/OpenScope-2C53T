/*
 * Host test for the autoset decisions (ui/autoset.c).
 *
 *   make test-autoset
 *
 * Ranges use my unit's tables (scope_cal.c), so the expected picks are
 * worked out from them in the comments; the trigger levels are EXP-89's
 * measured level/code pairs.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "autoset.h"
#include "move_mode.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define N 896u   /* 1024 minus the head skip, as the firmware passes */

static uint8_t rec[N];

static void sine(float period, float lo, float hi)
{
    for (unsigned i = 0; i < N; i++) {
        const float v = (lo + hi) / 2 + (hi - lo) / 2 * sinf(2.0f * 3.14159265f * (float)i / period);
        rec[i] = (uint8_t)(v + 0.5f);
    }
}

static void test_stats(void)
{
    autoset_stats_t st;
    sine(50.0f, 40.0f, 200.0f);
    autoset_stats(rec, N, &st);
    CHECK(st.lo <= 42 && st.lo >= 40 && st.hi >= 198 && st.hi <= 200, "sine levels %u..%u", st.lo, st.hi);
    CHECK(fabsf(st.mean - 120.0f) < 1.5f, "sine mean %.2f", st.mean);
    CHECK(!st.clipped, "an unclipped sine reads clipped");

    /* A quiet record with one glitch: the glitch must not set the level. */
    memset(rec, 48, N);
    for (unsigned i = 0; i < N; i += 7) rec[i] = 49;
    rec[300] = 250;
    autoset_stats(rec, N, &st);
    CHECK(st.hi <= 49, "one glitch set the top level to %u", st.hi);

    /* Clipping: a sine cut at the 227 ceiling. One sample at the rail is
     * a glitch, 2 % is clipping. */
    rec[300] = 48;
    rec[10] = 227;
    autoset_stats(rec, N, &st);
    CHECK(!st.clipped, "one sample at the rail counted as clipping");
    sine(50.0f, 40.0f, 260.0f);
    for (unsigned i = 0; i < N; i++) if (rec[i] > 227) rec[i] = 227;
    autoset_stats(rec, N, &st);
    CHECK(st.clipped, "a sine cut at 227 not seen as clipped");
    sine(50.0f, -40.0f, 100.0f);   /* wraps below 0 as uint8: force the floor */
    for (unsigned i = 0; i < N; i++) {
        const float v = 30.0f + 70.0f * sinf(2.0f * 3.14159265f * (float)i / 50.0f);
        rec[i] = (v < 0.0f) ? 0u : (uint8_t)(v + 0.5f);
    }
    autoset_stats(rec, N, &st);
    CHECK(st.clipped, "a sine cut at 0 not seen as clipped");
}

static void test_cycles(void)
{
    autoset_stats_t st;
    const float periods[] = { 8.0f, 50.0f, 137.3f, 400.0f };
    for (unsigned k = 0; k < 4; k++) {
        sine(periods[k], 60.0f, 160.0f);
        autoset_stats(rec, N, &st);
        const float cps = autoset_cycles_per_sample(rec, N, st.lo, st.hi);
        CHECK(fabsf(cps * periods[k] - 1.0f) < 0.03f, "period %.1f: %.5f cycles/sample", periods[k], cps);
    }
    /* Noise only (+/-1 count) and DC: nothing to count. */
    for (unsigned i = 0; i < N; i++) rec[i] = (uint8_t)(48 + (int)(i * 7u % 3u) - 1);
    autoset_stats(rec, N, &st);
    CHECK(autoset_cycles_per_sample(rec, N, st.lo, st.hi) == 0.0f, "noise counted as a signal");
    memset(rec, 120, N);
    CHECK(autoset_cycles_per_sample(rec, N, 120, 120) == 0.0f, "DC counted as a signal");
    /* Less than two cycles in the record: refuse rather than guess. */
    sine(2000.0f, 60.0f, 160.0f);
    autoset_stats(rec, N, &st);
    CHECK(autoset_cycles_per_sample(rec, N, st.lo, st.hi) == 0.0f, "half a cycle gave a rate");
    /* A noisy square wave still counts once per period (hysteresis). */
    for (unsigned i = 0; i < N; i++)
        rec[i] = (uint8_t)(((i / 40u) % 2u ? 180 : 50) + (int)(i * 13u % 5u) - 2);
    autoset_stats(rec, N, &st);
    const float cps = autoset_cycles_per_sample(rec, N, st.lo, st.hi);
    CHECK(fabsf(cps * 80.0f - 1.0f) < 0.03f, "square, period 80: %.5f", cps);
}

static void test_ranges(void)
{
    /* CH1, DAC1 2048 (0 V at 48.8 counts on r5, 44.7 on r7). */
    /* 0..3 V alone on the screen (5 div): r5, 3 / 0.640 = 4.7 div, top at 199. */
    CHECK(autoset_pick_range(1, 2048, 0.0f, 3.0f, 5.0f) == 5, "0-3 V single: r%u",
          autoset_pick_range(1, 2048, 0.0f, 3.0f, 5.0f));
    /* The same in a half (2.5 div): r6, 3 / 1.273 = 2.4 div. */
    CHECK(autoset_pick_range(1, 2048, 0.0f, 3.0f, 2.5f) == 6, "0-3 V split: r%u",
          autoset_pick_range(1, 2048, 0.0f, 3.0f, 2.5f));
    /* +/-2 V: 0 V sits low in the converter, so -2 V needs r7 (19 counts). */
    CHECK(autoset_pick_range(1, 2048, -2.0f, 2.0f, 5.0f) == 7, "+/-2 V: r%u",
          autoset_pick_range(1, 2048, -2.0f, 2.0f, 5.0f));
    /* 12 V DC with 100 mV ripple: the converter decides, r7 (top at 197). */
    CHECK(autoset_pick_range(1, 2048, 11.95f, 12.05f, 5.0f) == 7, "12 V DC: r%u",
          autoset_pick_range(1, 2048, 11.95f, 12.05f, 5.0f));
    /* 10 mV: the most sensitive calibrated range (range 0 since EXP-92:
     * 25 counts, 0.8 div). */
    CHECK(autoset_pick_range(2, 2114, 0.0f, 0.01f, 5.0f) == 0, "10 mV: r%u",
          autoset_pick_range(2, 2114, 0.0f, 0.01f, 5.0f));
    /* Nothing fits 100 V: the least sensitive range, not a refusal. */
    CHECK(autoset_pick_range(1, 2048, 0.0f, 100.0f, 5.0f) == 9, "100 V: r%u",
          autoset_pick_range(1, 2048, 0.0f, 100.0f, 5.0f));
    /* Bad channel: no calibration anywhere -> 9. */
    CHECK(autoset_pick_range(3, 2048, 0.0f, 1.0f, 5.0f) == 9, "ch3 picked a range");
}

static void test_timebase(void)
{
    /* 1 kHz: 3 cycles on 320 samples wants 107 kS/s: 0x0D (125 k, 2.6 cycles). */
    CHECK(autoset_pick_timebase(1000.0f) == 0x0D, "1 kHz: 0x%02X", autoset_pick_timebase(1000.0f));
    /* 20 kHz wants 2.1 MS/s: 0x09 (2.5 M, 2.6 cycles). */
    CHECK(autoset_pick_timebase(20000.0f) == 0x09, "20 kHz: 0x%02X", autoset_pick_timebase(20000.0f));
    /* 50 Hz wants 5.3 kS/s: 0x11 (5 k, 3.2 cycles). */
    CHECK(autoset_pick_timebase(50.0f) == 0x11, "50 Hz: 0x%02X", autoset_pick_timebase(50.0f));
    /* Beyond the ladder: the end of it, never an unmeasured code. */
    CHECK(autoset_pick_timebase(10e6f) == 0x08, "10 MHz: 0x%02X", autoset_pick_timebase(10e6f));
    CHECK(autoset_pick_timebase(0.5f) == 0x14, "0.5 Hz: 0x%02X", autoset_pick_timebase(0.5f));
    CHECK(autoset_pick_timebase(0.0f) == 0xFF, "no frequency must keep the timebase");
}

static void test_position_and_level(void)
{
    /* 100 counts above 0 V is 80.5 rows: move down by that much. */
    const int16_t p = autoset_position_rows(148.8f, 48.8f);
    CHECK(p == -80 || p == -81, "position %d", p);
    CHECK(autoset_position_rows(1000.0f, 48.8f) == -MOVE_VPOS_MAX, "clamp down");
    CHECK(autoset_position_rows(-1000.0f, 48.8f) == MOVE_VPOS_MAX, "clamp up");
    CHECK(autoset_position_rows(48.8f, 48.8f) == 0, "a signal centred on 0 V moved");

    /* EXP-89's measured pairs: level +9 -> code 139, -11 -> 115, +29 -> 164,
     * each firing at code - 28 = 111, 87, 136. */
    CHECK(autoset_trigger_level(111.0f) == 9, "mid 111: level %d", autoset_trigger_level(111.0f));
    CHECK(autoset_trigger_level(87.0f) == -11, "mid 87: level %d", autoset_trigger_level(87.0f));
    CHECK(autoset_trigger_level(136.0f) == 29, "mid 136: level %d", autoset_trigger_level(136.0f));
    /* Every code the UI level reaches (levels -100..100 give codes 4..252)
     * round-trips within one code; beyond them the level stops at its end. */
    int worst = 0;
    for (int mid = -24; mid <= 224; mid++) {
        const int lv = autoset_trigger_level((float)mid);
        const int code = mid + 28;
        const int got = 128 + (lv * 256) / 206;   /* fpga_trigger_code_from_level */
        const int e = abs(got - code);
        if (e > worst) worst = e;
    }
    CHECK(worst <= 1, "a level misses its code by %d", worst);
    CHECK(autoset_trigger_level(240.0f) == 100 && autoset_trigger_level(-60.0f) == -100,
          "beyond the reachable codes: %d / %d", autoset_trigger_level(240.0f),
          autoset_trigger_level(-60.0f));
}

static void test_signal(void)
{
    CHECK(!autoset_has_signal(0.010f, 0.002f), "10 mV of noise at 0 V is not a signal");
    CHECK(autoset_has_signal(0.030f, 0.0f), "30 mV swing is a signal");
    CHECK(autoset_has_signal(0.005f, 5.0f), "5 V DC is a signal");
    CHECK(autoset_has_signal(0.005f, -0.2f), "-0.2 V DC is a signal");
}

int main(void)
{
    test_stats();
    test_cycles();
    test_ranges();
    test_timebase();
    test_position_and_level();
    test_signal();
    if (failures) {
        printf("test_autoset: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_autoset: all passed\n");
    return 0;
}
