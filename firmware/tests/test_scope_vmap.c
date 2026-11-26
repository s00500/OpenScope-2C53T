/*
 * Host test for the vertical map (EXP-77): scope_vmap.c.
 *
 * Geometry is the real screen's: trace area rows 18..193 (the readout rows
 * start at 193), split at 105 when both channels are on, 206 rows per 256
 * counts at the calibrated scale (26 rows per 32-count division).
 *
 *   make test-scope-vmap
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "scope_vmap.h"

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

enum { TOP = 18, TRACE_BOT = 193, MID = 105, FULL_H = 206 };

/* EXP-77's grounded probe: CH1 records span 45..48 counts. With the old
 * 8-count floor one count was ~10 rows in the top split band and the noise
 * drew as a comb over a third of it. */
static void test_quiet_record_is_not_magnified(void)
{
    const int16_t bands[][2] = { { TOP, MID - 1 }, { MID + 1, TRACE_BOT }, { TOP, TRACE_BOT } };
    for (unsigned b = 0; b < 3; b++) {
        scope_vmap_t m;
        scope_vmap_autofit(&m, 45, 48, bands[b][0], bands[b][1], FULL_H);
        const int extent = scope_vmap_y(&m, 45) - scope_vmap_y(&m, 48);
        /* 3 counts at no more than MAX_ZOOM x 206/256 rows per count */
        const float cap = 3.0f * SCOPE_VMAP_MAX_ZOOM * FULL_H / 256.0f + 1.0f;
        CHECK(extent > 0 && extent <= cap,
              "band %u: a 3-count record spans %d rows (cap %.1f)", b, extent, (double)cap);
        /* centred, not pinned to the bottom edge */
        const int centre = (scope_vmap_y(&m, 45) + scope_vmap_y(&m, 48)) / 2;
        const int band_mid = (bands[b][0] + bands[b][1]) / 2;
        CHECK(abs(centre - band_mid) <= 3,
              "band %u: quiet record centred on row %d, band middle %d", b, centre, band_mid);
    }
}

static void test_large_record_fills_the_band(void)
{
    scope_vmap_t m;
    scope_vmap_autofit(&m, 20, 230, TOP, TRACE_BOT, FULL_H);
    CHECK(m.span == 210, "a 210-count record was re-spanned to %d", m.span);
    CHECK(scope_vmap_y(&m, 20) == TRACE_BOT - 2, "minimum on row %d", scope_vmap_y(&m, 20));
    CHECK(scope_vmap_y(&m, 230) <= TOP + 4, "maximum on row %d", scope_vmap_y(&m, 230));
}

static void test_rows_stay_in_the_band(void)
{
    const uint8_t recs[][2] = { { 0, 0 }, { 0, 1 }, { 254, 255 }, { 255, 255 }, { 45, 48 }, { 0, 255 } };
    const int16_t bands[][2] = { { TOP, MID - 1 }, { MID + 1, TRACE_BOT }, { TOP, TRACE_BOT } };
    for (unsigned r = 0; r < sizeof recs / sizeof recs[0]; r++)
        for (unsigned b = 0; b < 3; b++) {
            scope_vmap_t a, f;
            scope_vmap_autofit(&a, recs[r][0], recs[r][1], bands[b][0], bands[b][1], FULL_H);
            scope_vmap_fixed(&f, bands[b][0], bands[b][1], (int16_t)((bands[b][0] + bands[b][1]) / 2), 128, FULL_H);
            for (int v = 0; v < 256; v++) {
                const int ya = scope_vmap_y(&a, (uint8_t)v), yf = scope_vmap_y(&f, (uint8_t)v);
                if (ya < bands[b][0] || ya >= bands[b][1] || yf < bands[b][0] || yf >= bands[b][1]) {
                    CHECK(0, "record %u band %u: v=%d drawn on row %d / %d, outside [%d, %d)",
                          r, b, v, ya, yf, bands[b][0], bands[b][1]);
                    return;
                }
            }
        }
}

static void test_fixed_is_true_scale(void)
{
    scope_vmap_t m;
    scope_vmap_fixed(&m, TOP, TRACE_BOT, 121, 128, FULL_H);   /* 121 = grid centre */
    CHECK(scope_vmap_y(&m, 128) == 121, "centre code on row %d", scope_vmap_y(&m, 128));
    CHECK(abs((scope_vmap_y(&m, 96) - scope_vmap_y(&m, 128)) - 26) <= 1,
          "32 counts below centre is %d rows, a division is 26",
          scope_vmap_y(&m, 96) - scope_vmap_y(&m, 128));
    CHECK(scope_vmap_y(&m, 0) == TRACE_BOT - 1, "code 0 not clamped to the trace bottom");
    const float cpp = scope_vmap_counts_per_pixel(&m);
    CHECK(cpp > 1.24f && cpp < 1.25f, "fixed counts per pixel %.3f", (double)cpp);
}

static void test_counts_per_pixel_matches_the_drawing(void)
{
    scope_vmap_t m;
    scope_vmap_autofit(&m, 60, 200, TOP, TRACE_BOT, FULL_H);
    const float cpp = scope_vmap_counts_per_pixel(&m);
    const float drawn = 140.0f / (float)(scope_vmap_y(&m, 60) - scope_vmap_y(&m, 200));
    CHECK(cpp > 0.0f && drawn / cpp > 0.97f && drawn / cpp < 1.03f,
          "cursor scale %.3f counts/row, drawing %.3f", (double)cpp, (double)drawn);
    scope_vmap_t off = { 0 };
    CHECK(scope_vmap_counts_per_pixel(&off) == 0.0f, "an unset map reports a scale");
}

/* Vertical position (MOVE -> Position, 2026-11-26): every value moves by the
 * offset, the scale does not change, and the band clamp still holds. Both
 * map kinds, because the default (autofit) and true scale share the field. */
static void test_offset_moves_without_rescaling(void)
{
    scope_vmap_t a, b;
    scope_vmap_autofit(&a, 60, 120, TOP, TRACE_BOT, FULL_H);
    scope_vmap_autofit(&b, 60, 120, TOP, TRACE_BOT, FULL_H);
    scope_vmap_offset(&b, 13);
    int moved = 0;
    for (int v = 60; v <= 120; v += 10) {
        /* The record fills the band, so its top end clamps at the edge. */
        int want = scope_vmap_y(&a, (uint8_t)v) - 13;
        if (want < TOP) want = TOP;
        else moved++;
        CHECK(scope_vmap_y(&b, (uint8_t)v) == want,
              "autofit: value %d on row %d, want %d", v, scope_vmap_y(&b, (uint8_t)v), want);
    }
    CHECK(moved >= 5, "only %d values moved without clamping", moved);
    CHECK(scope_vmap_counts_per_pixel(&a) == scope_vmap_counts_per_pixel(&b),
          "the offset changed the scale");

    scope_vmap_fixed(&a, TOP, TRACE_BOT, MID, 128u, FULL_H);
    scope_vmap_fixed(&b, TOP, TRACE_BOT, MID, 128u, FULL_H);
    scope_vmap_offset(&b, -26);
    CHECK(scope_vmap_y(&b, 128u) == MID + 26, "fixed: centre moved to row %d", scope_vmap_y(&b, 128u));

    /* Pushed off the band: clamps to the edge, never leaves it. */
    scope_vmap_offset(&b, 400);
    for (int v = 0; v < 256; v += 15) {
        const int y = scope_vmap_y(&b, (uint8_t)v);
        CHECK(y >= TOP && y < TRACE_BOT, "offset 400: value %d on row %d", v, y);
    }

    /* A fresh fit starts with no offset (the struct is reset). */
    scope_vmap_autofit(&b, 60, 120, TOP, TRACE_BOT, FULL_H);
    CHECK(b.y_off == 0, "autofit kept a stale offset %d", b.y_off);
}

int main(void)
{
    test_offset_moves_without_rescaling();
    test_quiet_record_is_not_magnified();
    test_large_record_fills_the_band();
    test_rows_stay_in_the_band();
    test_fixed_is_true_scale();
    test_counts_per_pixel_matches_the_drawing();
    if (failures) {
        printf("test_scope_vmap: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_scope_vmap: all passed\n");
    return 0;
}
