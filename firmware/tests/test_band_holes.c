/*
 * Host test for ui/band_holes.h (EXP-85).
 *
 * The live compositor writes each band pixel exactly once per frame: either
 * the column stream writes it, or it lies in a hole a chip paints. This test
 * checks that property directly, pixel by pixel, for random hole sets
 * (overlapping, touching, past the band edges, zero-sized), plus the cases
 * the scope actually uses. A pixel written twice is a flash; a pixel written
 * zero times is stale trace left on the glass.
 *
 * Negative control: the same property run against a deliberately broken
 * segmenter (one that ignores overlaps) must FAIL, otherwise the checker is
 * not able to see the defect it exists for.
 *
 * Build/run:  make -C firmware test-band-holes
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "band_holes.h"

static int checks = 0, failed = 0;

#define CHECK(cond, ...) do {                       \
        checks++;                                   \
        if (!(cond)) {                              \
            failed++;                               \
            if (failed < 20) {                      \
                printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                printf(__VA_ARGS__);                \
                printf("\n");                       \
            }                                       \
        }                                           \
    } while (0)

typedef uint8_t (*segfn_t)(uint16_t, const band_rect_t *, uint8_t, uint16_t,
                           uint16_t, uint16_t *, uint16_t *);

/* Mutant: emits the gap before each hole without merging overlaps, so two
 * overlapping holes produce a segment inside the first one. */
static uint8_t broken_segments(uint16_t x, const band_rect_t *holes, uint8_t n_holes,
                               uint16_t top, uint16_t bot,
                               uint16_t *seg_top, uint16_t *seg_bot)
{
    uint16_t lo[BAND_MAX_HOLES], hi[BAND_MAX_HOLES];
    uint8_t n = 0;
    for (uint8_t i = 0; i < n_holes; i++) {
        const band_rect_t *r = &holes[i];
        if (!r->w || !r->h || x < r->x || x >= r->x + r->w) continue;
        uint16_t a = r->y < top ? top : r->y;
        uint16_t b = (r->y + r->h) > bot ? bot : (uint16_t)(r->y + r->h);
        if (a >= b) continue;
        uint8_t j = n;
        while (j && lo[j - 1] > a) { lo[j] = lo[j - 1]; hi[j] = hi[j - 1]; j--; }
        lo[j] = a; hi[j] = b; n++;
    }
    uint8_t ns = 0;
    uint16_t cur = top;
    for (uint8_t i = 0; i < n; i++) {
        if (lo[i] > cur) { seg_top[ns] = cur; seg_bot[ns] = lo[i]; ns++; }
        cur = hi[i];                         /* BUG: should be max(cur, hi) */
    }
    if (cur < bot) { seg_top[ns] = cur; seg_bot[ns] = bot; ns++; }
    return ns;
}

/* Every pixel of [0,w) x [top,bot) written exactly once by segments + holes.
 * Returns the number of bad pixels. */
static int coverage_errors(segfn_t fn, const band_rect_t *holes, uint8_t n,
                           uint16_t w, uint16_t top, uint16_t bot, int verbose)
{
    static uint8_t hits[240];
    int bad = 0;
    for (uint16_t x = 0; x < w; x++) {
        memset(hits, 0, sizeof(hits));
        uint16_t st[BAND_MAX_SEGS], sb[BAND_MAX_SEGS];
        uint8_t ns = fn(x, holes, n, top, bot, st, sb);
        uint16_t prev_bot = 0;
        for (uint8_t s = 0; s < ns; s++) {
            if (st[s] >= sb[s] || st[s] < top || sb[s] > bot ||
                (s > 0 && st[s] < prev_bot)) {
                if (verbose) printf("  bad segment x=%u [%u,%u)\n", x, st[s], sb[s]);
                bad++;
            }
            prev_bot = sb[s];
            for (uint16_t y = st[s]; y < sb[s] && y < 240; y++) hits[y]++;
        }
        for (uint16_t y = top; y < bot; y++) {
            int expect_stream = !band_in_hole(x, y, holes, n);
            if (hits[y] != (expect_stream ? 1 : 0)) {
                if (verbose && bad < 5)
                    printf("  x=%u y=%u written %u times, hole=%d\n",
                           x, y, hits[y], !expect_stream);
                bad++;
            }
        }
    }
    return bad;
}

static band_rect_t rnd_rect(void)
{
    band_rect_t r;
    r.x = (uint16_t)(rand() % 330);
    r.y = (uint16_t)(rand() % 250);
    r.w = (uint16_t)(rand() % 6 == 0 ? 0 : rand() % 120);
    r.h = (uint16_t)(rand() % 6 == 0 ? 0 : rand() % 60);
    return r;
}

int main(void)
{
    const uint16_t W = 320, TOP = 18, BOT = 193;

    /* 1. No holes: one segment, the whole column. */
    {
        uint16_t st[BAND_MAX_SEGS], sb[BAND_MAX_SEGS];
        uint8_t ns = band_col_segments(5, NULL, 0, TOP, BOT, st, sb);
        CHECK(ns == 1 && st[0] == TOP && sb[0] == BOT, "no holes: ns=%u", ns);
    }

    /* 2. The scope's own layout: popup, top-row chips, readout, cursor labels
     *    (positions as scope_ui.c computes them, including two that touch). */
    {
        band_rect_t h[] = {
            { 60, 102, 200, 36 },    /* popup */
            { 4, 20, 24, 12 },       /* CH label */
            { 40, 20, 100, 12 },     /* SPI3 diag */
            { 274, 20, 44, 12 },     /* trigger status */
            { 4, 34, 160, 12 },      /* readout line 1 */
            { 4, 46, 160, 12 },      /* readout line 2, touching line 1 */
            { 108, 180, 14, 12 },    /* V1 label at the band bottom */
            { 300, 60, 18, 12 },     /* H1 label at the right edge */
        };
        int e = coverage_errors(band_col_segments, h, 8, W, TOP, BOT, 1);
        CHECK(e == 0, "scope layout: %d bad pixels", e);
    }

    /* 3. A hole covering the whole column leaves no segment. */
    {
        band_rect_t h = { 0, 0, 320, 240 };
        uint16_t st[BAND_MAX_SEGS], sb[BAND_MAX_SEGS];
        CHECK(band_col_segments(7, &h, 1, TOP, BOT, st, sb) == 0, "full cover");
    }

    /* 4. Random hole sets. */
    srand(85);
    int rnd_bad = 0;
    for (int t = 0; t < 400; t++) {
        band_rect_t h[BAND_MAX_HOLES];
        uint8_t n = (uint8_t)(rand() % (BAND_MAX_HOLES + 1));
        for (uint8_t i = 0; i < n; i++) h[i] = rnd_rect();
        rnd_bad += coverage_errors(band_col_segments, h, n, W, TOP, BOT, 0) ? 1 : 0;
    }
    CHECK(rnd_bad == 0, "random hole sets: %d of 400 failed", rnd_bad);

    /* 5. Negative control: the broken segmenter must be caught. */
    {
        band_rect_t h[] = { { 10, 40, 50, 40 }, { 20, 50, 10, 10 } };  /* nested */
        int e = coverage_errors(broken_segments, h, 2, W, TOP, BOT, 0);
        CHECK(e > 0, "mutant (no overlap merge) was NOT caught");
    }

    printf("test_band_holes: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
