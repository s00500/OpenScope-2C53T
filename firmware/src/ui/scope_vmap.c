/*
 * OpenScope 2C53T: the vertical map. See scope_vmap.h (EXP-77).
 */
#include "scope_vmap.h"

#include <string.h>

void scope_vmap_autofit(scope_vmap_t *m, uint8_t mn, uint8_t mx,
                        int16_t y_top, int16_t y_bot, int full_h)
{
    memset(m, 0, sizeof *m);
    int h = (int)(y_bot - y_top) - 3;           /* usable height, small margin */
    if (h < 4) h = 4;
    if (full_h < 1) full_h = 1;

    /* Zoom cap: at most MAX_ZOOM x the calibrated rows per count, i.e. the
     * band must span at least h * 256 / (full_h * MAX_ZOOM) counts. */
    int min_span = (h * 256) / (full_h * SCOPE_VMAP_MAX_ZOOM);
    if (min_span < 8) min_span = 8;

    int lo = mn, span = (int)mx - (int)mn;
    if (span < min_span) {
        /* centre the record in the window it is given: (mn + mx - span) / 2,
         * floored (the window may start below code 0) */
        const int n = (int)mn + (int)mx - min_span;
        lo = (n >= 0) ? n / 2 : -((1 - n) / 2);
        span = min_span;
    }

    m->on     = true;
    m->y_top  = y_top;
    m->y_bot  = y_bot;
    m->h      = h;
    m->span   = span;
    m->mn     = (int16_t)lo;
    m->full_h = full_h;
}

void scope_vmap_fixed(scope_vmap_t *m, int16_t y_top, int16_t y_bot,
                      int16_t y_mid, uint8_t center, int full_h)
{
    memset(m, 0, sizeof *m);
    m->on     = true;
    m->fixed  = true;
    m->y_top  = y_top;
    m->y_bot  = y_bot;
    m->y_mid  = y_mid;
    m->center = center;
    m->full_h = full_h < 1 ? 1 : full_h;
}

void scope_vmap_offset(scope_vmap_t *m, int16_t rows)
{
    m->y_off = rows;
}

int16_t scope_vmap_y(const scope_vmap_t *m, uint8_t v)
{
    int yy;
    if (m->fixed)
        yy = (int)m->y_mid - ((int)v - (int)m->center) * m->full_h / 256;
    else
        yy = (int)(m->y_bot - 2) - ((int)v - (int)m->mn) * m->h / m->span;
    yy -= m->y_off;
    if (yy < m->y_top)  yy = m->y_top;
    if (yy >= m->y_bot) yy = m->y_bot - 1;
    return (int16_t)yy;
}

float scope_vmap_counts_per_pixel(const scope_vmap_t *m)
{
    if (!m->on) return 0.0f;
    if (m->fixed) return 256.0f / (float)m->full_h;
    if (m->h <= 0) return 0.0f;
    return (float)m->span / (float)m->h;
}
