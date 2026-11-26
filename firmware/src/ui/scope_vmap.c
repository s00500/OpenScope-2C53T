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
    m->c_top  = y_top;
    m->c_bot  = y_bot;
    m->h      = h;
    m->span   = span;
    m->mn     = (int16_t)lo;
    m->full_h = full_h;
}

void scope_vmap_fixed(scope_vmap_t *m, int16_t y_top, int16_t y_bot,
                      int16_t y_mid, int16_t center, int full_h)
{
    memset(m, 0, sizeof *m);
    m->on     = true;
    m->fixed  = true;
    m->y_top  = y_top;
    m->y_bot  = y_bot;
    m->c_top  = y_top;
    m->c_bot  = y_bot;
    m->y_mid  = y_mid;
    m->center = center;
    m->full_h = full_h < 1 ? 1 : full_h;
}

void scope_vmap_offset(scope_vmap_t *m, int16_t rows)
{
    m->y_off = rows;
}

void scope_vmap_clip(scope_vmap_t *m, int16_t c_top, int16_t c_bot)
{
    if (c_bot <= c_top)
        return;                 /* nonsense: keep the band */
    m->c_top = c_top;
    m->c_bot = c_bot;
}

int16_t scope_vmap_y(const scope_vmap_t *m, uint8_t v)
{
    int yy;
    if (m->fixed)
        yy = (int)m->y_mid - ((int)v - (int)m->center) * m->full_h / 256;
    else
        yy = (int)(m->y_bot - 2) - ((int)v - (int)m->mn) * m->h / m->span;
    yy -= m->y_off;
    if (yy < m->c_top)  yy = m->c_top;
    if (yy >= m->c_bot) yy = m->c_bot - 1;
    return (int16_t)yy;
}

int16_t scope_vmap_y_counts(const scope_vmap_t *m, float v)
{
    /* The integer path's arithmetic in float: the offset in rows is
     * truncated toward zero exactly as C's integer division is, so a whole
     * count lands on the same row. Bounded first: a zero far off the window
     * must clamp, not overflow the cast. */
    float d;
    int   base;
    if (m->fixed) {
        d = (v - (float)m->center) * (float)m->full_h / 256.0f;
        base = m->y_mid;
    } else {
        d = (v - (float)m->mn) * (float)m->h / (float)m->span;
        base = m->y_bot - 2;
    }
    if (d >  4096.0f) d =  4096.0f;
    if (d < -4096.0f) d = -4096.0f;
    int yy = base - (int)d - m->y_off;
    if (yy < m->c_top)  yy = m->c_top;
    if (yy >= m->c_bot) yy = m->c_bot - 1;
    return (int16_t)yy;
}

int16_t scope_vmap_ref_y(const scope_vmap_t *m)
{
    int yy = m->fixed ? (int)m->y_mid : (int)(m->y_bot - 2) - m->h / 2;
    yy -= m->y_off;
    if (yy < m->c_top)  yy = m->c_top;
    if (yy >= m->c_bot) yy = m->c_bot - 1;
    return (int16_t)yy;
}

float scope_vmap_counts_per_pixel(const scope_vmap_t *m)
{
    if (!m->on) return 0.0f;
    if (m->fixed) return 256.0f / (float)m->full_h;
    if (m->h <= 0) return 0.0f;
    return (float)m->span / (float)m->h;
}
