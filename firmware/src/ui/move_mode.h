/*
 * OpenScope 2C53T: what the arrow keys do in the scope's time view (MOVE).
 *
 * MOVE cycles the arrows through modes, and each mode owns BOTH axes:
 *
 *   Scale     UP/DOWN volts/div of the active channel, LEFT/RIGHT time/div
 *   Trigger   UP/DOWN trigger level,  LEFT/RIGHT trigger point (H position)
 *   Position  UP/DOWN vertical position of the active channel,
 *             LEFT/RIGHT trigger point (H position)
 *   Mask      mask tolerances, SELECT stop-on-fail (only while a mask exists)
 *   Cursors   UP/DOWN move the active cursor, LEFT/RIGHT pick the next one
 *             (only while cursors are on)
 *
 * Until 2026-11-26 the stages split the axes: "Trig level" left LEFT/RIGHT on
 * the timebase and "Position" left UP/DOWN on volts/div, there was no
 * vertical position at all, and cursors took all four arrows whenever they
 * were on. The mode is shown on the glass as a tag in the top row, so the
 * arrows never do something the screen does not say.
 *
 * PURE C99, NO FIRMWARE DEPENDENCIES: tests/test_move_mode.c drives it on the
 * host (make test-move-mode).
 */
#ifndef MOVE_MODE_H
#define MOVE_MODE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MOVE_SCALE = 0,     /* the default */
    MOVE_TRIGGER,
    MOVE_POSITION,
    MOVE_MASK,          /* only while a mask exists */
    MOVE_CURSORS,       /* only while cursors are on */
    MOVE_MODE_COUNT
} move_mode_t;

/* One press moves half a grid division on either axis: the grid is 26 rows
 * per vertical division and 32 columns per horizontal one (scope_ui.c). */
#define MOVE_ROWS_PER_DIV 26
#define MOVE_COLS_PER_DIV 32
#define MOVE_VPOS_STEP   13     /* rows */
#define MOVE_VPOS_MAX   130     /* rows either way: five divisions, from the
                                   middle of either half of a split screen to
                                   the far edge (was 78, three, 2026-11-26) */
#define MOVE_HPOS_STEP   16     /* columns */
#define MOVE_HPOS_MIN     8     /* trigger point column limits */
#define MOVE_HPOS_MAX   312

static inline bool move_mode_offered(move_mode_t m, bool mask, bool cursors)
{
    if (m == MOVE_MASK)    return mask;
    if (m == MOVE_CURSORS) return cursors;
    return m < MOVE_MODE_COUNT;
}

/* The mode in force: a stored mode whose condition went away (the mask was
 * cleared, the cursors switched off) falls back to Scale. */
static inline move_mode_t move_mode_effective(move_mode_t m, bool mask, bool cursors)
{
    return move_mode_offered(m, mask, cursors) ? m : MOVE_SCALE;
}

/* MOVE: the next offered mode, Scale -> Trigger -> Position -> [Mask] ->
 * [Cursors] -> Scale. */
static inline move_mode_t move_mode_next(move_mode_t m, bool mask, bool cursors)
{
    m = move_mode_effective(m, mask, cursors);
    for (int i = 1; i <= (int)MOVE_MODE_COUNT; i++) {
        const move_mode_t n = (move_mode_t)(((int)m + i) % (int)MOVE_MODE_COUNT);
        if (move_mode_offered(n, mask, cursors))
            return n;
    }
    return MOVE_SCALE;
}

/* The tag in the top row. */
static inline const char *move_mode_name(move_mode_t m)
{
    switch (m) {
    case MOVE_SCALE:    return "Scale";
    case MOVE_TRIGGER:  return "Trigger";
    case MOVE_POSITION: return "Position";
    case MOVE_MASK:     return "Mask";
    case MOVE_CURSORS:  return "Cursors";
    default:            return "?";
    }
}

/* What the arrows do, for the popup MOVE raises (fits the 31-char popup). */
static inline const char *move_mode_hint(move_mode_t m)
{
    switch (m) {
    case MOVE_SCALE:    return "UP/DN V/div  LT/RT time/div";
    case MOVE_TRIGGER:  return "UP/DN level  LT/RT trig point";
    case MOVE_POSITION: return "UP/DN pos  LT/RT trig point";
    case MOVE_MASK:     return "Arrows tolerance  SEL stop";
    case MOVE_CURSORS:  return "UP/DN move  LT/RT next cursor";
    default:            return "";
    }
}

/* Vertical position after one press (dir +1 = UP = the trace moves up),
 * clamped to +/- MOVE_VPOS_MAX rows. */
static inline int16_t move_vpos_step(int16_t pos, int dir)
{
    int p = (int)pos + (dir > 0 ? MOVE_VPOS_STEP : -MOVE_VPOS_STEP);
    if (p >  MOVE_VPOS_MAX) p =  MOVE_VPOS_MAX;
    if (p < -MOVE_VPOS_MAX) p = -MOVE_VPOS_MAX;
    return (int16_t)p;
}

/* Trigger point column after one press (dir +1 = RIGHT). */
static inline int16_t move_hpos_step(int16_t x, int dir)
{
    int p = (int)x + (dir > 0 ? MOVE_HPOS_STEP : -MOVE_HPOS_STEP);
    if (p < MOVE_HPOS_MIN) p = MOVE_HPOS_MIN;
    if (p > MOVE_HPOS_MAX) p = MOVE_HPOS_MAX;
    return (int16_t)p;
}

/* An offset in pixels as grid divisions with one decimal, e.g. "+1.5" or
 * "-0.3" (integer maths: the firmware's printf has no %f). */
static inline void move_fmt_div(char *out, unsigned n, int px, int px_per_div)
{
    const int neg = px < 0;
    int t = ((neg ? -px : px) * 10 + px_per_div / 2) / px_per_div;   /* tenths */
    if (n < 2u) { if (n) out[0] = '\0'; return; }
    /* "+12.3" at most for the ranges used here */
    char b[12];
    int i = 0;
    b[i++] = (t == 0) ? ' ' : (neg ? '-' : '+');
    const int whole = t / 10, tenth = t % 10;
    if (whole >= 10) b[i++] = (char)('0' + (whole / 10) % 10);
    b[i++] = (char)('0' + whole % 10);
    b[i++] = '.';
    b[i++] = (char)('0' + tenth);
    b[i] = '\0';
    unsigned k = 0;
    for (const char *s = (b[0] == ' ') ? b + 1 : b; *s && k + 1u < n; s++)
        out[k++] = *s;
    out[k] = '\0';
}

#endif /* MOVE_MODE_H */
