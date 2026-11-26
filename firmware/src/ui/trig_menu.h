/*
 * OpenScope 2C53T: the trigger panel, and what the channel keys do
 *
 * Model adapted from deOliveira-R's parity C3 (f4dfc23, "trigger panel, source
 * on reg 0x06"), which we have not merged: it builds on their key model.
 *
 * Stock's TRIGGER key opens "Trigger Settings" (menu table 0x0804C824) with
 * three entries: "Mode:" Normal/Auto/Single (0x0804C7F0..F8), "Edge:" and
 * "Channel:" CH1/CH2 (0x0804C7FC/C800). Here TRIGGER, in the scope's time
 * view, opens the same panel. Keys while it is open: UP/DOWN pick the row,
 * LEFT/RIGHT change it (OK = RIGHT), TRIGGER or MENU close it, any other key
 * closes it too (and does nothing else).
 *
 * The cursors moved from TRIGGER to SELECT (the user, 2026-11-26), and the
 * mask key from AUTO to the panel's fourth row, "Mask:" (the user,
 * 2026-11-26, when AUTO became an autoset): LEFT/RIGHT/OK on it teaches a
 * mask, cancels a teach, or turns the mask off, as AUTO used to.
 *
 * PURE: no LCD, no FPGA. tests/test_trig_menu.c runs this code. scope_ui.c
 * draws the panel; input_handler.c applies the hardware side of a change
 * (reg 0x06 for the source) from the bits trig_menu_change() returns.
 */
#ifndef TRIG_MENU_H
#define TRIG_MENU_H

#include <stdint.h>
#include <stdbool.h>
#include "scope_state.h"

typedef enum {
    TM_ROW_MODE = 0,
    TM_ROW_EDGE,
    TM_ROW_SOURCE,
    TM_ROW_MASK,            /* the mask key: teach / cancel / off */
    TM_ROW_COUNT
} trig_menu_row_t;

typedef struct {
    bool    open;
    uint8_t row;            /* trig_menu_row_t */
} trig_menu_t;

#define TM_CHANGED_MODE    0x01u
#define TM_CHANGED_EDGE    0x02u
#define TM_CHANGED_SOURCE  0x04u
#define TM_MASK_KEY        0x08u    /* the Mask row: the caller runs the mask key */
#define TM_REFUSED_OFF     0x80u    /* the source asked for is a channel that is off */

/* The one panel: the input task writes it, the display task reads it. */
extern trig_menu_t scope_trig_menu;

void trig_menu_open(trig_menu_t *m);       /* opens on the Mode row, as stock */
void trig_menu_close(trig_menu_t *m);
void trig_menu_row_step(trig_menu_t *m, int dir);    /* wraps */

/* Change the selected row by dir (+1/-1, wraps). Returns TM_CHANGED_* for
 * what moved, or TM_REFUSED_OFF (nothing changed) when the source would land
 * on a disabled channel: stock keeps the source on a live channel too, and
 * flips it when one is switched off (0x080235E8). */
uint8_t trig_menu_change(const trig_menu_t *m, scope_state_t *ss, int dir);

const char *trig_menu_row_label(uint8_t row);        /* "Mode:", "Edge:", "Channel:", "Mask:" */
const char *trig_menu_row_value(const scope_state_t *ss, uint8_t row);   /* "" for Mask */

/* The Mask row's value from the mask's state (0 = none, 1 = teaching,
 * 2 = testing: mask_pf_state_t's order): "Off", "Teach 3/8", "On". */
void trig_menu_mask_value(uint8_t state, uint16_t got, uint16_t target,
                          char *out, uint32_t n);

/* ── CH1 / CH2 keys (the user's design, 2026-11-26) ──────────────────────
 *
 * A channel key selects its channel (what UP/DOWN and the other per-channel
 * controls act on); pressed again on the selected channel it switches the
 * channel off; on a channel that is off it switches it on and selects it.
 * The last channel on cannot be switched off. AC/DC coupling is no longer on
 * these keys; it stays in Settings -> Oscilloscope. */
typedef enum {
    CHKEY_SELECT = 0,       /* make it the active channel */
    CHKEY_TURN_ON,          /* switch on, and make it active */
    CHKEY_TURN_OFF,         /* switch off (it was active and on) */
    CHKEY_REFUSE_LAST,      /* it is the only channel on: nothing changes */
} chan_key_action_t;

/* ch: 0 = CH1, 1 = CH2. active: the active channel (0/1). */
chan_key_action_t chan_key_decide(uint8_t ch, uint8_t active,
                                  bool ch1_on, bool ch2_on);

#endif /* TRIG_MENU_H */
