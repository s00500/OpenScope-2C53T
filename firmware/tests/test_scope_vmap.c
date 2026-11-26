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

/* The channel reference marker's row: on the trace's centre in autofit, on
 * code 128 in true scale, and it moves with the position. */
static void test_reference_row(void)
{
    scope_vmap_t m;
    scope_vmap_autofit(&m, 40, 200, TOP, TRACE_BOT, FULL_H);
    const int mid = (scope_vmap_y(&m, 40) + scope_vmap_y(&m, 200)) / 2;
    CHECK(abs(scope_vmap_ref_y(&m) - mid) <= 1,
          "autofit reference on row %d, trace centre %d", scope_vmap_ref_y(&m), mid);
    const int r0 = scope_vmap_ref_y(&m);
    scope_vmap_offset(&m, 26);
    CHECK(scope_vmap_ref_y(&m) == r0 - 26, "offset 26: reference %d, was %d",
          scope_vmap_ref_y(&m), r0);
    scope_vmap_offset(&m, -500);
    CHECK(scope_vmap_ref_y(&m) == TRACE_BOT - 1, "pushed down: %d", scope_vmap_ref_y(&m));

    scope_vmap_fixed(&m, TOP, TRACE_BOT, MID, 128u, FULL_H);
    CHECK(scope_vmap_ref_y(&m) == scope_vmap_y(&m, 128u), "fixed: reference %d, code 128 on %d",
          scope_vmap_ref_y(&m), scope_vmap_y(&m, 128u));
    scope_vmap_offset(&m, -13);
    CHECK(scope_vmap_ref_y(&m) == scope_vmap_y(&m, 128u), "fixed, offset: %d vs %d",
          scope_vmap_ref_y(&m), scope_vmap_y(&m, 128u));
}

/* The 0 V marker (EXP-89) reads a fractional count through the same map:
 * whole counts land on scope_vmap_y's row in every configuration, fractions
 * fall between their neighbours, and a zero far off the window clamps to the
 * band edge (below the window: the last row). */
static void test_fractional_counts(void)
{
    scope_vmap_t ms[4];
    scope_vmap_autofit(&ms[0], 40, 200, TOP, TRACE_BOT, FULL_H);
    scope_vmap_autofit(&ms[1], 45, 48, TOP, MID - 1, FULL_H);      /* quiet, zoomed */
    scope_vmap_fixed(&ms[2], TOP, TRACE_BOT, MID, 128u, FULL_H);
    scope_vmap_fixed(&ms[3], MID + 1, TRACE_BOT, (MID + 1 + TRACE_BOT) / 2, 128u, FULL_H);
    scope_vmap_offset(&ms[3], 13);
    for (unsigned k = 0; k < 4; k++) {
        const scope_vmap_t *m = &ms[k];
        int bad = 0;
        for (int v = 0; v <= 255; v++)
            if (scope_vmap_y_counts(m, (float)v) != scope_vmap_y(m, (uint8_t)v)) bad++;
        CHECK(bad == 0, "map %u: %d whole counts on a different row", k, bad);
        for (int v = 0; v < 255; v++) {
            const int a = scope_vmap_y(m, (uint8_t)v), b = scope_vmap_y(m, (uint8_t)(v + 1));
            const int f = scope_vmap_y_counts(m, (float)v + 0.5f);
            if (!(f <= a && f >= b)) { bad++; break; }
        }
        CHECK(bad == 0, "map %u: a half count is not between its neighbours", k);
        CHECK(scope_vmap_y_counts(m, -1.0e6f) == m->y_bot - 1, "map %u: far below -> %d", k,
              scope_vmap_y_counts(m, -1.0e6f));
        CHECK(scope_vmap_y_counts(m, 1.0e6f) == m->y_top, "map %u: far above -> %d", k,
              scope_vmap_y_counts(m, 1.0e6f));
    }
    /* Zoomed in, a fraction moves the row: 45.0 and 46.5 differ. */
    CHECK(scope_vmap_y_counts(&ms[1], 45.0f) > scope_vmap_y_counts(&ms[1], 46.5f) + 3,
          "zoomed: 45.0 on %d, 46.5 on %d", scope_vmap_y_counts(&ms[1], 45.0f),
          scope_vmap_y_counts(&ms[1], 46.5f));
}

/* Overlay (2026-11-26): with both channels on, CH1's band is the top half.
 * Moved down by Position with the clip widened to the whole trace area, its
 * rows go past the band into the bottom half and stop only at the area's
 * edge; without the clip (the control) they flatten on the band edge. The
 * scale and the unmoved rows are the band's either way. */
static void test_clip_lets_a_moved_trace_cross_the_band(void)
{
    scope_vmap_t band, wide;
    scope_vmap_autofit(&band, 60, 120, TOP, MID - 1, FULL_H);
    scope_vmap_autofit(&wide, 60, 120, TOP, MID - 1, FULL_H);
    scope_vmap_clip(&wide, TOP, TRACE_BOT);

    /* Unmoved: identical rows, all inside the band. */
    for (int v = 60; v <= 120; v += 5)
        CHECK(scope_vmap_y(&wide, (uint8_t)v) == scope_vmap_y(&band, (uint8_t)v) &&
              scope_vmap_y(&wide, (uint8_t)v) < MID - 1,
              "unmoved value %d: %d vs %d", v, scope_vmap_y(&wide, (uint8_t)v),
              scope_vmap_y(&band, (uint8_t)v));
    CHECK(scope_vmap_counts_per_pixel(&wide) == scope_vmap_counts_per_pixel(&band),
          "the clip changed the scale");

    /* Five divisions down (130 rows). */
    scope_vmap_offset(&band, -130);
    scope_vmap_offset(&wide, -130);
    int below = 0, flat = 0;
    for (int v = 60; v <= 120; v += 5) {
        const int yw = scope_vmap_y(&wide, (uint8_t)v), yb = scope_vmap_y(&band, (uint8_t)v);
        if (yw >= MID + 1) below++;
        if (yb == MID - 2) flat++;
        CHECK(yw >= TOP && yw < TRACE_BOT, "value %d left the area: row %d", v, yw);
    }
    CHECK(below == 13, "only %d of 13 values crossed into the bottom half", below);
    CHECK(flat == 13, "control: only %d of 13 values flattened on the band edge", flat);
    /* The lowest value sits on the area's last row, not past it. */
    CHECK(scope_vmap_y(&wide, 60) == TRACE_BOT - 1, "bottom: row %d", scope_vmap_y(&wide, 60));

    /* The fractional lookup and the reference row use the same clip. */
    CHECK(scope_vmap_y_counts(&wide, 90.0f) == scope_vmap_y(&wide, 90),
          "fractional %d vs %d", scope_vmap_y_counts(&wide, 90.0f), scope_vmap_y(&wide, 90));
    CHECK(scope_vmap_ref_y(&wide) > MID, "reference row %d stayed in the top half",
          scope_vmap_ref_y(&wide));

    /* A fresh fit resets the clip to the band; a nonsense clip is ignored. */
    scope_vmap_autofit(&wide, 60, 120, TOP, MID - 1, FULL_H);
    CHECK(wide.c_top == TOP && wide.c_bot == MID - 1, "fit kept a stale clip");
    scope_vmap_clip(&wide, 150, 100);
    CHECK(wide.c_top == TOP && wide.c_bot == MID - 1, "accepted an empty clip");
}

int main(void)
{
    test_clip_lets_a_moved_trace_cross_the_band();
    test_fractional_counts();
    test_reference_row();
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
