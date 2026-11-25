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
 * Rows outside [y_top, y_bot) are never returned: a value beyond the band
 * clamps to its edge. Free of LCD and RTOS dependencies; host-tested by
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
    int16_t y_top, y_bot;  /* drawable rows: [y_top, y_bot) */
    int     h, span;       /* autofit: usable rows, counts they cover */
    int16_t mn;            /* autofit: the count drawn at the bottom (may be < 0) */
    int16_t y_mid;         /* fixed: the row `center` is drawn on */
    uint8_t center;        /* fixed: ADC code at y_mid */
    int     full_h;        /* rows per 256 counts at the calibrated scale */
} scope_vmap_t;

/* Autofit over a record whose samples lie in [mn, mx]. */
void scope_vmap_autofit(scope_vmap_t *m, uint8_t mn, uint8_t mx,
                        int16_t y_top, int16_t y_bot, int full_h);

/* True scale: `center` on row y_mid, full_h rows per 256 counts. */
void scope_vmap_fixed(scope_vmap_t *m, int16_t y_top, int16_t y_bot,
                      int16_t y_mid, uint8_t center, int full_h);

/* The row for ADC value v, clamped into [y_top, y_bot). */
int16_t scope_vmap_y(const scope_vmap_t *m, uint8_t v);

/* ADC counts one row is worth (for the cursor readout); 0 if not set up. */
float scope_vmap_counts_per_pixel(const scope_vmap_t *m);

#endif /* SCOPE_VMAP_H */
