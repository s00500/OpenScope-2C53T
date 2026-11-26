/*
 * OpenScope 2C53T: the vertical map, ADC count -> screen row (EXP-77).
 *
 * Every renderer of a scope trace goes through this: the full repaint, the
 * live column compositor and the trigger marker. Two copies of a transform
 * that are supposed to agree will not stay agreeing (scope_ui.c has the
 * history), and until EXP-77 the compositor still had its own: it always
 * autofitted, so with the true-scale graticule on the trace jumped between
 * two scales whenever the renderer changed.
 *
 * Two modes:
 *   autofit  the band spans the record's own min..max, but never zoomed past
 *            SCOPE_VMAP_MAX_ZOOM x the calibrated scale; a record quieter
 *            than that is centred in its band, not pinned to the bottom edge.
 *   fixed    true volts/div: `full_h` rows per 256 counts about `center`, the
 *            geometry the grid and the volts/div label assume.
 *
 * The band [y_top, y_bot) sets the scale and where the trace starts; the
 * clip [c_top, c_bot) is where it may be drawn. They are the same until
 * scope_vmap_clip() widens the clip (2026-11-26: with both channels on, a
 * channel moved by Position may cross the other's half, as on a bench
 * scope). Rows outside the clip are never returned: a value beyond it clamps
 * to its edge. Free of LCD and RTOS dependencies; host-tested by
 * tests/test_scope_vmap.c.
 */
#ifndef SCOPE_VMAP_H
#define SCOPE_VMAP_H

#include <stdbool.h>
#include <stdint.h>

/* Autofit never draws one ADC count taller than this many times its
 * calibrated height. The old floor (a span of 8 counts) made one count about
 * 12 rows in a split band, so a grounded probe's +/-1 count of ADC noise drew
 * as a comb across a third of the band. */
#define SCOPE_VMAP_MAX_ZOOM 4

typedef struct {
    bool    on;
    bool    fixed;
    int16_t y_top, y_bot;  /* the band: rows the fit / scale is set in */
    int16_t c_top, c_bot;  /* the clip: rows a value may land on (default the band) */
    int     h, span;       /* autofit: usable rows, counts they cover */
    int16_t mn;            /* autofit: the count drawn at the bottom (may be < 0) */
    int16_t y_mid;         /* fixed: the row `center` is drawn on */
    int16_t center;        /* fixed: ADC count at y_mid (a 0 V count may lie
                              outside 0..255, so signed) */
    int     full_h;        /* rows per 256 counts at the calibrated scale */
    int16_t y_off;         /* vertical position: rows up (MOVE -> Position) */
} scope_vmap_t;

/* Autofit over a record whose samples lie in [mn, mx]. */
void scope_vmap_autofit(scope_vmap_t *m, uint8_t mn, uint8_t mx,
                        int16_t y_top, int16_t y_bot, int full_h);

/* True scale: `center` on row y_mid, full_h rows per 256 counts. */
void scope_vmap_fixed(scope_vmap_t *m, int16_t y_top, int16_t y_bot,
                      int16_t y_mid, int16_t center, int full_h);

/* Vertical position: draw every value `rows` higher (negative = lower).
 * Applied after the fit, so it moves the trace without rescaling it, and
 * clamped like everything else: a trace moved past the band edge flattens
 * on it, the way a scope trace goes off the glass. Every renderer reads the
 * row through scope_vmap_y(), so the trace, the trigger level line and the
 * mask bounds move together. Call after scope_vmap_autofit/fixed (they
 * reset it). */
void scope_vmap_offset(scope_vmap_t *m, int16_t rows);

/* Let values land on [c_top, c_bot) instead of the band only: the scale and
 * the start row stay the band's. Call after scope_vmap_autofit/fixed (they
 * reset it to the band). */
void scope_vmap_clip(scope_vmap_t *m, int16_t c_top, int16_t c_bot);

/* The row for ADC value v, clamped into the clip [c_top, c_bot). */
int16_t scope_vmap_y(const scope_vmap_t *m, uint8_t v);

/* The same row for a fractional count (a calibrated 0 V, EXP-89), with the
 * same clamp: below the window it is the band's last row, above it the
 * first. scope_vmap_y(m, v) == scope_vmap_y_counts(m, v) for whole counts. */
int16_t scope_vmap_y_counts(const scope_vmap_t *m, float v);

/* The channel's reference row, where its marker on the left edge points:
 * autofit, the middle of the fitted window (the trace is centred on it);
 * fixed, the row of `center` (the channel's 0 V count where it is known,
 * EXP-89; code 128 otherwise). Moves with the vertical position, clamped into
 * the band like every other row. Not a measured 0 V: where the zero is
 * known, the marker uses scope_vmap_y_counts(zero) instead (scope_ui.c). */
int16_t scope_vmap_ref_y(const scope_vmap_t *m);

/* ADC counts one row is worth (for the cursor readout); 0 if not set up. */
float scope_vmap_counts_per_pixel(const scope_vmap_t *m);

#endif /* SCOPE_VMAP_H */
