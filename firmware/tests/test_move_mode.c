/*
 * Host test for ui/move_mode.h: what MOVE hands the arrow keys (2026-11-26).
 *
 * Negative controls: the Mask and Cursors modes must never be offered
 * without a mask / with the cursors off, and a stored mode whose condition
 * went away must fall back to Scale; each is also run against a deliberately
 * wrong cycle, which must fail it.
 *
 * Build/run:  make -C firmware test-move-mode
 */
#include <stdio.h>
#include <string.h>

#include "move_mode.h"

static int checks = 0, failed = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { failed++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef move_mode_t (*next_fn)(move_mode_t, bool, bool);

/* Mutant: the plain modulo cycle, which offers every mode always. */
static move_mode_t mutant_next(move_mode_t m, bool mask, bool cursors)
{
    (void)mask; (void)cursors;
    return (move_mode_t)(((int)m + 1) % (int)MOVE_MODE_COUNT);
}

/* Walk the cycle from Scale for one lap; returns how many presses it takes
 * to come back (0 if it never does within 10) and records the order. */
static int lap(next_fn f, bool mask, bool cursors, move_mode_t order[10])
{
    move_mode_t m = MOVE_SCALE;
    for (int i = 0; i < 10; i++) {
        m = f(m, mask, cursors);
        order[i] = m;
        if (m == MOVE_SCALE) return i + 1;
    }
    return 0;
}

/* Errors in the cycle rules, for one implementation. */
static int cycle_errors(next_fn f)
{
    int bad = 0;
    move_mode_t o[10];

    /* Neither: Scale -> Trigger -> Position -> Scale. */
    if (lap(f, false, false, o) != 3 || o[0] != MOVE_TRIGGER || o[1] != MOVE_POSITION) bad++;
    /* Mask only: Mask after Position. */
    if (lap(f, true, false, o) != 4 || o[2] != MOVE_MASK) bad++;
    /* Cursors only: Cursors after Position. */
    if (lap(f, false, true, o) != 4 || o[2] != MOVE_CURSORS) bad++;
    /* Both. */
    if (lap(f, true, true, o) != 5 || o[2] != MOVE_MASK || o[3] != MOVE_CURSORS) bad++;
    /* Never offered without its condition, from any start. */
    for (int s = 0; s < (int)MOVE_MODE_COUNT; s++) {
        const move_mode_t n = f((move_mode_t)s, false, false);
        if (n == MOVE_MASK || n == MOVE_CURSORS) bad++;
    }
    return bad;
}

int main(void)
{
    /* The cycle, and the mutant must fail it. */
    const int e = cycle_errors(move_mode_next);
    CHECK(e == 0, "move_mode_next breaks %d cycle rules", e);
    const int em = cycle_errors(mutant_next);
    CHECK(em > 0, "mutant (modes always offered) was NOT caught");

    /* A stale mode falls back to Scale. */
    CHECK(move_mode_effective(MOVE_MASK, false, true) == MOVE_SCALE, "mask cleared under Mask");
    CHECK(move_mode_effective(MOVE_CURSORS, true, false) == MOVE_SCALE, "cursors off under Cursors");
    CHECK(move_mode_effective(MOVE_POSITION, false, false) == MOVE_POSITION, "Position kept");
    /* ...and MOVE from a stale mode continues from Scale, not from it. */
    CHECK(move_mode_next(MOVE_CURSORS, false, false) == MOVE_TRIGGER,
          "MOVE after the cursors went off: %d", move_mode_next(MOVE_CURSORS, false, false));

    /* Every mode has a tag and a hint that fit: tag <= 8 chars, hint <= 31
     * (popup_text is 32 bytes). */
    for (int m = 0; m < (int)MOVE_MODE_COUNT; m++) {
        CHECK(strlen(move_mode_name((move_mode_t)m)) <= 8u, "tag %d too long", m);
        CHECK(strlen(move_mode_hint((move_mode_t)m)) > 0u &&
              strlen(move_mode_hint((move_mode_t)m)) <= 31u, "hint %d length", m);
    }

    /* Vertical position: half a division per press, clamped both ways. */
    int16_t p = 0;
    p = move_vpos_step(p, +1);
    CHECK(p == MOVE_VPOS_STEP, "one UP: %d", p);
    for (int i = 0; i < 20; i++) p = move_vpos_step(p, +1);
    CHECK(p == MOVE_VPOS_MAX, "clamped up: %d", p);
    for (int i = 0; i < 40; i++) p = move_vpos_step(p, -1);
    CHECK(p == -MOVE_VPOS_MAX, "clamped down: %d", p);

    /* Horizontal: from mid-screen, half a division per press. */
    int16_t x = 160;
    x = move_hpos_step(x, -1);
    CHECK(x == 144, "one LEFT: %d", x);
    for (int i = 0; i < 30; i++) x = move_hpos_step(x, -1);
    CHECK(x == MOVE_HPOS_MIN, "clamped left: %d", x);
    for (int i = 0; i < 30; i++) x = move_hpos_step(x, +1);
    CHECK(x == MOVE_HPOS_MAX, "clamped right: %d", x);

    /* Divisions text. */
    char b[16];
    move_fmt_div(b, sizeof b, 13, 26);   CHECK(strcmp(b, "+0.5") == 0, "13/26: %s", b);
    move_fmt_div(b, sizeof b, -78, 26);  CHECK(strcmp(b, "-3.0") == 0, "-78/26: %s", b);
    move_fmt_div(b, sizeof b, 0, 26);    CHECK(strcmp(b, "0.0") == 0, "0: %s", b);
    move_fmt_div(b, sizeof b, -152, 32); CHECK(strcmp(b, "-4.8") == 0, "-152/32: %s", b);
    move_fmt_div(b, sizeof b, 152, 32);  CHECK(strcmp(b, "+4.8") == 0, "152/32: %s", b);
    move_fmt_div(b, 3, 152, 32);         CHECK(strcmp(b, "+4") == 0, "truncated: %s", b);

    printf("test_move_mode: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
