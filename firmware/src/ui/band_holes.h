/*
 * OpenScope 2C53T: holes in the live trace band
 *
 * The live compositor (draw_scope_live_frame) streams the trace band one
 * column at a time and writes every pixel's final colour, so the screen never
 * holds a blank state. It cannot recompute glyphs per pixel, so anything drawn
 * with text over the band (the popup, the label chips at the top, the cursor
 * labels and the cursor readout) is a HOLE: the column is emitted as the
 * segments between the holes, and the text is painted into its hole as an
 * opaque box. Every pixel of the band is then written exactly once per frame,
 * by either the column stream or a chip, and nothing is ever cleared first.
 *
 * Until 2026-11-26 the only hole was the popup, so every other label in the
 * band was overwritten by the first live frame, and the cursors were not drawn
 * by the compositor at all: with any cursor on, the scope fell back to the
 * clear-and-repaint path at ~2.3 repaints/s, measured on my unit (EXP-85).
 *
 * PURE C99, NO FIRMWARE DEPENDENCIES: tests/test_band_holes.c drives it on the
 * host (make test-band-holes).
 */

#ifndef BAND_HOLES_H
#define BAND_HOLES_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint16_t x, y, w, h;     /* screen pixels; w == 0 or h == 0 is "no hole" */
} band_rect_t;

/* Most segments one column can split into: one more than the holes that can
 * cross it. The scope's chip set is small (see BAND_MAX_HOLES). */
#define BAND_MAX_HOLES   18u
#define BAND_MAX_SEGS    (BAND_MAX_HOLES + 1u)

/*
 * The rows of column x in [top, bot) that are NOT covered by any hole, as
 * half-open segments [seg_top[i], seg_bot[i]) in increasing order, never
 * empty, never overlapping. Holes may overlap each other, extend past the
 * band, or miss it. Returns the number of segments (0 when the holes cover
 * the whole column).
 */
static inline uint8_t band_col_segments(uint16_t x, const band_rect_t *holes,
                                        uint8_t n_holes, uint16_t top, uint16_t bot,
                                        uint16_t *seg_top, uint16_t *seg_bot)
{
    if (bot <= top) return 0u;
    if (n_holes > BAND_MAX_HOLES) n_holes = BAND_MAX_HOLES;

    /* The holes that cross this column, clamped to the band, as [lo, hi). */
    uint16_t lo[BAND_MAX_HOLES], hi[BAND_MAX_HOLES];
    uint8_t n = 0u;
    for (uint8_t i = 0u; i < n_holes; i++) {
        const band_rect_t *r = &holes[i];
        if (r->w == 0u || r->h == 0u) continue;
        if (x < r->x || x >= (uint32_t)r->x + r->w) continue;
        uint32_t a = r->y, b = (uint32_t)r->y + r->h;
        if (a < top) a = top;
        if (b > bot) b = bot;
        if (a >= b) continue;
        /* Insertion sort by start; n is at most a dozen. */
        uint8_t j = n;
        while (j > 0u && lo[j - 1u] > a) {
            lo[j] = lo[j - 1u];
            hi[j] = hi[j - 1u];
            j--;
        }
        lo[j] = (uint16_t)a;
        hi[j] = (uint16_t)b;
        n++;
    }

    /* Complement of the (merged) holes within [top, bot). */
    uint8_t ns = 0u;
    uint16_t cur = top;
    for (uint8_t i = 0u; i < n; i++) {
        if (lo[i] > cur) {
            seg_top[ns] = cur;
            seg_bot[ns] = lo[i];
            ns++;
        }
        if (hi[i] > cur) cur = hi[i];
    }
    if (cur < bot) {
        seg_top[ns] = cur;
        seg_bot[ns] = bot;
        ns++;
    }
    return ns;
}

/* True if (x, y) lies in any hole. For tests and for single-pixel shapes. */
static inline bool band_in_hole(uint16_t x, uint16_t y,
                                const band_rect_t *holes, uint8_t n_holes)
{
    for (uint8_t i = 0u; i < n_holes; i++) {
        const band_rect_t *r = &holes[i];
        if (x >= r->x && x < (uint32_t)r->x + r->w &&
            y >= r->y && y < (uint32_t)r->y + r->h)
            return true;
    }
    return false;
}

#endif /* BAND_HOLES_H */
