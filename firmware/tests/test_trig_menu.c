/*
 * Host test for ui/trig_menu.c: the trigger panel model and the CH1/CH2 key
 * decision (EXP-86). The same code the input task runs.
 *
 * Negative controls: a source change onto a switched-off channel must be
 * refused with nothing changed, and the last channel on must not be switched
 * off; each assertion is also run against a deliberately wrong decision
 * function, which must fail it.
 *
 * Build/run:  make -C firmware test-trig-menu
 */
#include <stdio.h>
#include <string.h>

#include "trig_menu.h"

static int checks = 0, failed = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { failed++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Mutant: the obvious-but-wrong toggle (second press always switches off). */
static chan_key_action_t mutant_decide(uint8_t ch, uint8_t active, bool c1, bool c2)
{
    const bool on = ch ? c2 : c1;
    if (!on) return CHKEY_TURN_ON;
    if (active != ch) return CHKEY_SELECT;
    return CHKEY_TURN_OFF;
}

typedef chan_key_action_t (*decide_fn)(uint8_t, uint8_t, bool, bool);

/* The rules, as a table: returns the number of rows the function gets wrong. */
static int chan_key_table_errors(decide_fn f)
{
    static const struct { uint8_t ch, active; bool c1, c2; chan_key_action_t want; } t[] = {
        { 0, 1, true,  true,  CHKEY_SELECT },       /* CH1 key, CH2 active: select CH1 */
        { 0, 0, true,  true,  CHKEY_TURN_OFF },     /* CH1 key on active CH1: off */
        { 0, 0, true,  false, CHKEY_REFUSE_LAST },  /* the only channel on stays on */
        { 0, 1, false, true,  CHKEY_TURN_ON },      /* CH1 off: on (and selected) */
        { 1, 0, true,  true,  CHKEY_SELECT },
        { 1, 1, true,  true,  CHKEY_TURN_OFF },
        { 1, 1, false, true,  CHKEY_REFUSE_LAST },
        { 1, 0, true,  false, CHKEY_TURN_ON },
        { 1, 1, true,  false, CHKEY_TURN_ON },      /* active but off: on */
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (f(t[i].ch, t[i].active, t[i].c1, t[i].c2) != t[i].want) bad++;
    return bad;
}

int main(void)
{
    scope_state_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.ch1.enabled = true;
    ss.ch2.enabled = true;
    ss.trigger.mode = TRIG_AUTO;
    ss.trigger.edge = TRIG_RISING;
    ss.trigger.source = TRIG_SRC_CH1;

    trig_menu_t m = { false, 2 };

    /* Opens on Mode, as stock; rows wrap both ways. */
    trig_menu_open(&m);
    CHECK(m.open && m.row == TM_ROW_MODE, "open: row %u", m.row);
    trig_menu_row_step(&m, -1);
    CHECK(m.row == TM_ROW_SOURCE, "UP from Mode wraps to Channel: %u", m.row);
    trig_menu_row_step(&m, +1);
    CHECK(m.row == TM_ROW_MODE, "DOWN from Channel wraps to Mode: %u", m.row);

    /* Mode cycles Auto -> Normal -> Single -> Auto, and back. */
    CHECK(trig_menu_change(&m, &ss, +1) == TM_CHANGED_MODE && ss.trigger.mode == TRIG_NORMAL,
          "mode +1: %d", ss.trigger.mode);
    trig_menu_change(&m, &ss, +1);
    trig_menu_change(&m, &ss, +1);
    CHECK(ss.trigger.mode == TRIG_AUTO, "mode wraps: %d", ss.trigger.mode);
    trig_menu_change(&m, &ss, -1);
    CHECK(ss.trigger.mode == TRIG_SINGLE, "mode -1 wraps: %d", ss.trigger.mode);
    CHECK(strcmp(trig_menu_row_value(&ss, TM_ROW_MODE), "Single") == 0, "mode label");

    /* Edge. */
    m.row = TM_ROW_EDGE;
    CHECK(trig_menu_change(&m, &ss, +1) == TM_CHANGED_EDGE && ss.trigger.edge == TRIG_FALLING,
          "edge: %d", ss.trigger.edge);

    /* Source: CH1 -> CH2 with both on. */
    m.row = TM_ROW_SOURCE;
    CHECK(trig_menu_change(&m, &ss, +1) == TM_CHANGED_SOURCE && ss.trigger.source == TRIG_SRC_CH2,
          "source: %d", ss.trigger.source);
    CHECK(strcmp(trig_menu_row_value(&ss, TM_ROW_SOURCE), "CH2") == 0, "source label");

    /* NEGATIVE: CH1 off, source CH2: moving to CH1 is refused, nothing changes. */
    ss.ch1.enabled = false;
    CHECK(trig_menu_change(&m, &ss, +1) == TM_REFUSED_OFF && ss.trigger.source == TRIG_SRC_CH2,
          "source onto an off channel: %d", ss.trigger.source);
    ss.ch1.enabled = true;

    CHECK(strcmp(trig_menu_row_label(TM_ROW_SOURCE), "Channel:") == 0, "row label");
    trig_menu_close(&m);
    CHECK(!m.open, "close");

    /* CH1/CH2 keys. */
    int e = chan_key_table_errors(chan_key_decide);
    CHECK(e == 0, "chan_key_decide gets %d rows wrong", e);
    int em = chan_key_table_errors(mutant_decide);
    CHECK(em > 0, "mutant (no last-channel guard) was NOT caught");

    printf("test_trig_menu: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
