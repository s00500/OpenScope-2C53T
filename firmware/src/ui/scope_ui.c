/*
 * OpenScope 2C53T - Oscilloscope UI
 *
 * Waveform display with:
 *   - Trigger level arrow + dotted line
 *   - Ground reference markers (CH1/CH2 zero-volt arrows)
 *   - Trigger status badge (Auto/Trig'd/Ready/Stop)
 *   - Run/Stop indicator
 *   - Auto-measurement badges (Freq, Vpp, Vrms, Duty, Period, Rise)
 *   - Quick-change popup overlay
 */

#include "ui.h"
#include "lcd.h"
#include "font.h"
#include "theme.h"
#include "scope_state.h"
#include "scope_measure.h"
#include "scope_cal.h"
#include "scope_timebase.h"
#include "scope_cursor.h"
#include "scope_freq.h"
#include "scope_record.h"
#include "math_channel.h"
#include "persistence.h"
#include "fpga.h"
#include "FreeRTOS.h"   /* pvPortMalloc — the X-Y snapshot buffers */
#include "task.h"       /* xTaskGetTickCount — the out-of-scope popup overlay */
#include "scope_trigger.h"
#include "trig_edge.h"
#include "scope_mask.h"
#include "scope_vmap.h"
#include "band_holes.h"
#include "trig_menu.h"
#ifdef FEATURE_FFT
#include "fft_live.h"
#include "fft_test_signals.h"
#endif
#include "at32f403a_407.h"  /* GPIO port reads in the debug overlay */
#include <stdio.h>
#include <math.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════
 * Layout constants
 * ═══════════════════════════════════════════════════════════════════ */

#define SCOPE_TOP       18      /* Below status bar */
#define SCOPE_BOT       (LCD_HEIGHT - 16)  /* Above info bar */
#define SCOPE_H         (SCOPE_BOT - SCOPE_TOP)
#define SCOPE_MID_Y     (SCOPE_TOP + SCOPE_H / 2)
#ifdef SCOPE_DEBUG_OVERLAY
#define SCOPE_DBG_Y     (LCD_HEIGHT - 60)  /* top of the debug strip */
#endif

/* Measurement badge layout */
#define BADGE_W         76
#define BADGE_H         14
#define BADGE_PAD       2
/* Badge rows.
 *
 * BENCH 2026-08-14: with SCOPE_DEBUG_OVERLAY the badges were INVISIBLE, and had
 * been in every build — `-DSCOPE_DEBUG_OVERLAY` is unconditional in the
 * Makefile's C_DEFS, so there was no configuration in which they showed. The
 * overlay's four lines start at SCOPE_DBG_Y (=180) and run to ~219, painting
 * straight over rows that sat at 193 and 208.
 *
 * Debug builds therefore stack the badges ABOVE the debug strip. That costs
 * waveform height, which is the right trade: an overlay build is a bench build,
 * and a measurement you cannot see is worth less than the pixels it hides. */
#ifdef SCOPE_DEBUG_OVERLAY
#define BADGE_ROW_Y     (SCOPE_DBG_Y - BADGE_H - 2)
#define BADGE_ROW2_Y    (BADGE_ROW_Y - BADGE_H - 1)
#else
#define BADGE_ROW_Y     (SCOPE_BOT - BADGE_H - 2)
#define BADGE_ROW2_Y    (BADGE_ROW_Y - BADGE_H - 1)
#endif

/* The trace area ends where the readout rows begin (EXP-77). Until then the
 * full repaint drew traces down to SCOPE_BOT, through the readout rows, while
 * the live compositor only repaints down to BADGE_ROW2_Y: trace pixels in the
 * readout strip were never cleared and sat under the text until the next full
 * repaint. Every trace transform now ends here; the grid (and so the
 * true-scale geometry) still spans SCOPE_TOP..SCOPE_BOT. */
#define TRACE_BOT       BADGE_ROW2_Y
#define TRACE_MID_Y     (SCOPE_TOP + (TRACE_BOT - SCOPE_TOP) / 2)

/*
 * How many consecutive refusals the Freq badge holds its last reading for.
 * At the measured ~46% refusal rate a hold of 8 frames makes a dropout
 * visible only when the signal genuinely goes away, while still expiring
 * fast enough that a removed probe blanks the badge within a second.
 */
#define FREQ_HOLD_FRAMES  8u

/* Quick-change popup: centred, sized to its text (popup_geom). It used to be
 * a fixed 200 px box with the text centred in font_large and no width check,
 * so a longer message ("SAVE: not in this build", ~23 characters) ran past
 * both edges of the box and the trace overpainted its ends. */

/* ═══════════════════════════════════════════════════════════════════
 * Quick-change popup state (shared with main.c via extern)
 * ═══════════════════════════════════════════════════════════════════ */

static char popup_text[32] = "";
static uint8_t popup_frames = 0;  /* Countdown: show for N frames then dismiss */

/* Has the box been painted for THIS popup? The incremental path paints it
 * once and then leaves it alone (see draw_scope_live_frame's hole); the full
 * path repaints it every frame anyway. Without this, every live frame would
 * repaint the box and the text would strobe at the capture rate. */
static bool popup_painted = false;

#define POPUP_DURATION 10  /* ~500ms at 20fps */

/* Popups outside the scope (2026-10-06, softkey UI P0; findings F15, F20).
 * The scope's popup is counted in scope frames, and only the scope renderer
 * counted them -- so a popup raised in the meter, siggen or settings never
 * appeared there, and turned up stale on the next scope visit. Outside the
 * scope it is now a tick-timed overlay the display task services, sized to
 * its text instead of a fixed 200 px box. */
#define POPUP_OVERLAY_MS 1200u
static bool     ov_pending;
static bool     ov_drawn;
static uint32_t ov_until;

void scope_show_popup(const char *text)
{
    int i = 0;
    while (text[i] && i < (int)sizeof(popup_text) - 1) {
        popup_text[i] = text[i];
        i++;
    }
    popup_text[i] = '\0';
    if (current_mode == MODE_OSCILLOSCOPE) {
        popup_frames = POPUP_DURATION;
        popup_painted = false;
    } else {
        popup_frames = 0;
        ov_pending = true;
        ov_drawn = false;
        ov_until = xTaskGetTickCount() + pdMS_TO_TICKS(POPUP_OVERLAY_MS);
    }
}

/* The popup box for the current text: font_large if it fits, else
 * font_medium, the box 24 px wider than the text, centred on the screen. The
 * scope popup and the out-of-scope overlay share it, and the live compositor
 * cuts its hole from the same rectangle (popup_rect). */
static const font_t *popup_geom(uint16_t *px, uint16_t *py, uint16_t *pw, uint16_t *ph)
{
    const font_t *f = &font_large;
    uint16_t tw = font_string_width(popup_text, f);
    if (tw > LCD_WIDTH - 28) { f = &font_medium; tw = font_string_width(popup_text, f); }
    uint16_t w = tw + 24;
    if (w > LCD_WIDTH - 4) w = LCD_WIDTH - 4;
    uint16_t h = (uint16_t)(f->height + 12);
    *px = (uint16_t)((LCD_WIDTH - w) / 2);
    *py = (uint16_t)((LCD_HEIGHT - h) / 2);
    *pw = w;
    *ph = h;
    return f;
}

static band_rect_t popup_rect(void)
{
    band_rect_t r;
    (void)popup_geom(&r.x, &r.y, &r.w, &r.h);
    return r;
}

/* Writes every pixel of the box once (border, fills, string box), so it can
 * be painted over a live trace without a blank frame. */
static void draw_overlay_box(void)
{
    const theme_t *th = theme_get();
    uint16_t x, y, w, h;
    const font_t *f = popup_geom(&x, &y, &w, &h);
    lcd_fill_rect(x, y, w, 1, th->highlight);
    lcd_fill_rect(x, y + h - 1, w, 1, th->highlight);
    lcd_fill_rect(x, y + 1, 1, h - 2, th->highlight);
    lcd_fill_rect(x + w - 1, y + 1, 1, h - 2, th->highlight);
    lcd_fill_rect(x + 1, y + 1, w - 2, 5, th->background);
    font_draw_string_box(x + 1, y + 6, w - 2, popup_text, th->text_primary,
                         th->background, f, FONT_ALIGN_CENTER);
    lcd_fill_rect(x + 1, y + 6 + f->height, w - 2, (uint16_t)(h - 7 - f->height), th->background);
}

int ui_popup_overlay_service(bool screen_repainted)
{
    if (!ov_pending) return 0;
    if ((int32_t)(xTaskGetTickCount() - ov_until) >= 0) {
        ov_pending = false;
        ov_drawn = false;
        return 2;                       /* expired: caller repaints the screen */
    }
    if (!ov_drawn || screen_repainted) {
        draw_overlay_box();
        ov_drawn = true;
        return 1;
    }
    return 0;
}

void ui_popup_overlay_cancel(void)
{
    ov_pending = false;
    ov_drawn = false;
}

bool scope_popup_active(void)
{
    return popup_frames > 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * Grid
 * ═══════════════════════════════════════════════════════════════════ */

void draw_scope_grid(void)
{
    const theme_t *th = theme_get();
    uint16_t x, y;

    /* Vertical grid lines (dotted) */
    for (x = 0; x < LCD_WIDTH; x += 32) {
        for (y = SCOPE_TOP; y < SCOPE_BOT; y += 2)
            lcd_set_pixel(x, y, th->grid);
    }
    /* Horizontal grid lines (dotted) */
    for (y = SCOPE_TOP; y < SCOPE_BOT; y += 26) {
        for (x = 0; x < LCD_WIDTH; x += 2)
            lcd_set_pixel(x, y, th->grid);
    }

    /* Center crosshair (solid) */
    for (x = 0; x < LCD_WIDTH; x++)
        lcd_set_pixel(x, SCOPE_MID_Y, th->grid_center);
    for (y = SCOPE_TOP; y < SCOPE_BOT; y++)
        lcd_set_pixel(LCD_WIDTH / 2, y, th->grid_center);
}

/* ═══════════════════════════════════════════════════════════════════
 * Trigger level indicator
 * ═══════════════════════════════════════════════════════════════════ */

/* trig_y comes from trigger_marker_y_*() below: the row where the HARDWARE
 * fires, not `level` pixels above centre (which is what this drew until
 * 2026-09-22, unrelated to an autofit trace). */
/* The right-edge trigger arrow (pointing left at the level) and the small
 * edge arrow beside it, as a pixel test: true if (x, y) is part of the shape.
 * Both renderers draw it through this, the full path over a bounding box and
 * the live compositor per pixel. It was full-path only until 2026-11-26, so
 * the first live frame after any repaint erased it. */
static bool trig_arrow_px(int x, int y, int trig_y, bool rising)
{
    const int ax = LCD_WIDTH - 1;
    const int dx = ax - x;          /* 0 at the right edge */
    const int dy = y - trig_y;
    if (dx < 0 || dx > 7 || dy < -2 || dy > 2) return false;

    /* Level arrow: a 5 px shaft with two diagonal barbs. */
    if (dy == 0 && dx <= 4) return true;
    if ((dx == 1 && (dy == -1 || dy == 1)) || (dx == 2 && (dy == -2 || dy == 2)))
        return true;

    /* Edge arrow at ax-6: a 4 px stem pointing up (rising) or down, with a
     * crossbar at the level. */
    if (dx == 6) return rising ? (dy >= -1) : (dy <= 1);
    if ((dx == 5 || dx == 7) && dy == 0) return true;
    return false;
}

/* trig_y comes from trigger_marker_y_*() below: the row where the HARDWARE
 * fires, not `level` pixels above centre (which is what this drew until
 * 2026-09-22, unrelated to an autofit trace). */
static void draw_trigger_indicator(const scope_state_t *ss, const theme_t *th,
                                   int16_t trig_y)
{
    if (trig_y < SCOPE_TOP + 2) trig_y = SCOPE_TOP + 2;
    if (trig_y > TRACE_BOT - 3) trig_y = TRACE_BOT - 3;

    const bool rising = (ss->trigger.edge == TRIG_RISING);
    for (int y = trig_y - 2; y <= trig_y + 2; y++)
        for (int x = LCD_WIDTH - 8; x < LCD_WIDTH; x++)
            if (trig_arrow_px(x, y, trig_y, rising))
                lcd_set_pixel((uint16_t)x, (uint16_t)y, th->trigger);

    /* Dotted horizontal line at trigger level; denser while UP/DOWN are
     * adjusting it, so the user can see which control the arrows own. */
    for (uint16_t x = 0; x < LCD_WIDTH - 6; x += (scope_move_mode_now() == MOVE_TRIGGER ? 2u : 4u))
        lcd_set_pixel(x, (uint16_t)trig_y, th->trigger);
}

/* ═══════════════════════════════════════════════════════════════════
 * Measurement badges
 * ═══════════════════════════════════════════════════════════════════ */

/* One badge: label in the dim colour on the left, value in the channel
 * colour on the right. Every pixel of the box is written once, text as
 * opaque boxes (font_draw_string_box), so a refresh never blanks the old
 * text first. It used to fill the box and then draw the text, which is a
 * visible blink when the badges are refreshed on their own (a volts/div or
 * time/div change, 2026-11-26). */
static void draw_one_badge(uint16_t x, uint16_t y, const char *label,
                           const char *value, uint16_t color, const theme_t *th)
{
    const font_t *f = &font_small;
    const uint16_t ty = (uint16_t)(y + 1u);
    const uint16_t inner = BADGE_W - 2u * BADGE_PAD;
    uint16_t lw = font_string_width(label, f);
    if (lw > inner) lw = inner;

    lcd_fill_rect(x, y, BADGE_W, 1, th->background);
    lcd_fill_rect(x, ty, BADGE_PAD, f->height, th->background);
    font_draw_string_box((uint16_t)(x + BADGE_PAD), ty, lw, label,
                         th->text_secondary, th->background, f, FONT_ALIGN_LEFT);
    font_draw_string_box((uint16_t)(x + BADGE_PAD + lw), ty, (uint16_t)(inner - lw), value,
                         color, th->background, f, FONT_ALIGN_RIGHT);
    lcd_fill_rect((uint16_t)(x + BADGE_W - BADGE_PAD), ty, BADGE_PAD, f->height,
                  th->background);
    if (BADGE_H > 1u + f->height)
        lcd_fill_rect(x, (uint16_t)(ty + f->height), BADGE_W,
                      (uint16_t)(BADGE_H - 1u - f->height), th->background);
}

/* A full-width text slot of badge row 2, written once per pixel (see
 * draw_one_badge). */
static void draw_slot_text(uint16_t x, uint16_t y, const char *text, uint16_t color,
                           const theme_t *th)
{
    const font_t *f = &font_small;
    const uint16_t w = (uint16_t)(LCD_WIDTH - x);
    lcd_fill_rect(x, y, w, 1, th->background);
    font_draw_string_box(x, (uint16_t)(y + 1u), w, text, color, th->background, f,
                         FONT_ALIGN_LEFT);
    if (BADGE_H > 1u + f->height)
        lcd_fill_rect(x, (uint16_t)(y + 1u + f->height), w,
                      (uint16_t)(BADGE_H - 1u - f->height), th->background);
}

/* Shown wherever the instrument cannot measure the quantity today. */
#define MEAS_NA  "--"

/*
 * Measure one channel's live capture record.
 *
 * Returns false — and leaves *m zeroed — unless the FPGA has actually
 * delivered samples. fpga_data_ready() is the same gate the waveform plot
 * uses (see draw_demo_waveform), so the badges and the trace can never
 * disagree about whether what is on screen is real.
 */
static bool measure_live_channel(const volatile uint8_t *buf,
                                 scope_measure_t *m)
{
    memset(m, 0, sizeof(*m));
    if (!fpga_data_ready() || buf == NULL)
        return false;

    /* Tear-checked read (audit 2026-08-20, P0.2). fpga.c now stages each
     * capture and commits it in a ~10 µs window bracketed by the frame
     * generation counter (odd = commit in progress); a commit landing inside
     * our analysis pass would splice two frames through every statistic, so
     * re-run the pass when the generation moved. Commits arrive at ≤ ~34 Hz,
     * so the second attempt virtually always stands; after three, accept the
     * torn result (pre-fix behavior) rather than spin. The volatile cast is
     * safe because the generation check is what detects concurrent writes. */
    for (int attempt = 0; attempt < 3; attempt++) {
        uint32_t g0 = fpga_acq_frame_generation();
        scope_measure_record((const uint8_t *)buf, FPGA_ADC_BUF_SIZE, m);
        if ((g0 & 1u) == 0u && fpga_acq_frame_generation() == g0)
            break;
    }
    return m->valid;
}

/* No float formatting: the firmware links newlib-nano without
 * -u _printf_float, so "%f" prints nothing. Tenths are assembled by hand
 * here, and in scope_cursor.c for the cursor readout. */
static void fmt_counts(char *b, size_t n, unsigned v)
{
    snprintf(b, n, "%ucnt", v);
}

static void fmt_tenths(char *b, size_t n, float v, const char *unit)
{
    if (v < 0.0f) v = 0.0f;
    unsigned t = (unsigned)(v * 10.0f + 0.5f);
    snprintf(b, n, "%u.%u%s", t / 10u, t % 10u, unit);
}

/*
 * Frequency with automatic Hz/kHz/MHz ranging, integer math only.
 * Same constraint as fmt_volts: newlib-nano's "%f" prints nothing.
 */
__attribute__((unused))
static void fmt_hz(char *b, size_t n, float hz)
{
    if (hz < 0.0f) hz = 0.0f;
    if (hz < 1000.0f) {
        const unsigned tenths = (unsigned)(hz * 10.0f + 0.5f);
        snprintf(b, n, "%u.%uHz", tenths / 10u, tenths % 10u);
    } else if (hz < 1000000.0f) {
        const unsigned hundredths = (unsigned)(hz / 10.0f + 0.5f);
        snprintf(b, n, "%u.%02ukHz", hundredths / 100u, hundredths % 100u);
    } else {
        const unsigned hundredths = (unsigned)(hz / 10000.0f + 0.5f);
        snprintf(b, n, "%u.%02uMHz", hundredths / 100u, hundredths % 100u);
    }
}

/*
 * A duration in seconds, ranged us/ms/s. Integer math, as above.
 */
static void fmt_seconds(char *b, size_t n, float s)
{
    if (s < 0.0f) s = 0.0f;
    if (s < 1e-3f) {
        snprintf(b, n, "%uus", (unsigned)(s * 1e6f + 0.5f));
    } else if (s < 1.0f) {
        const unsigned hundredths_ms = (unsigned)(s * 1e5f + 0.5f);
        snprintf(b, n, "%u.%02ums", hundredths_ms / 100u, hundredths_ms % 100u);
    } else {
        const unsigned hundredths = (unsigned)(s * 100.0f + 0.5f);
        snprintf(b, n, "%u.%02us", hundredths / 100u, hundredths % 100u);
    }
}

/*
 * Format a voltage with automatic mV/V ranging, integer math only (same
 * reason as fmt_tenths: newlib-nano's "%f" prints nothing). Sub-volt values
 * read as whole millivolts ("154mV"); at/above 1 V, two decimals ("1.66V").
 * Rounding is done once, in millivolts, so 0.9995 V shows "1.00V" rather than
 * the nonsense "1000mV".
 */
static void fmt_volts(char *b, size_t n, float v)
{
    if (v < 0.0f) v = 0.0f;
    unsigned mv = (unsigned)(v * 1000.0f + 0.5f);   /* millivolts, rounded */
    if (mv < 1000u)
        snprintf(b, n, "%umV", mv);
    else
        snprintf(b, n, "%u.%02uV", mv / 1000u, (mv % 1000u) / 10u);
}

/*
 * Vertical calibration now lives in scope_cal.c, per CHANNEL as well as per
 * range, because the two channels have different frontends and measure
 * different gains on the same range index.
 *
 * WITHDRAWN 2026-08-18 — what used to be here was a two-case switch:
 *     case 2: 1.0f/347.0f   ("20mV/div")
 *     case 8: 1.0f/154.0f   ("2V/div")
 * The five-amplitude sweep of 2026-08-17 contradicts both. Range 2 rails on
 * both channels and yields no span at all; range 8 measures 279 mV/count, not
 * 6.49. The full provenance and the reason the replacement is tiered rather
 * than a flat table is in scope_cal.h.
 *
 * The compile-time check below is what keeps the volts/div label honest.
 * SCOPE_CAL_COUNTS_PER_DIV is a claim about THIS renderer's geometry — the
 * 26-pixel grid pitch against a 206-pixel plot showing 256 counts. If either
 * changes, the label silently starts meaning something else, so make that a
 * build failure instead.
 */
_Static_assert((26 * 256 + SCOPE_H / 2) / SCOPE_H == (int)SCOPE_CAL_COUNTS_PER_DIV,
               "grid pitch no longer matches SCOPE_CAL_COUNTS_PER_DIV — "
               "the volts/div labels in the status bar would be wrong");

/* The cal table is indexed by the frontend range index, so its length and
 * the UI's range count are the same quantity counted twice. */
_Static_assert(SCOPE_CAL_RANGE_COUNT == VDIV_COUNT,
               "scope_cal range table length must match VDIV_COUNT");

/* The horizontal counterpart: one sample per screen column, vertical grid
 * every 32 px, so a division is 32 samples. If either changes, the seconds/div
 * label and every frequency silently start meaning something else. */
_Static_assert(SCOPE_TIMEBASE_SAMPLES_PER_DIV == 32.0f,
               "grid pitch no longer matches SCOPE_TIMEBASE_SAMPLES_PER_DIV");
_Static_assert(SCOPE_TIMEBASE_CODE_COUNT == TIMEBASE_COUNT,
               "timebase rate table length must match TIMEBASE_COUNT");

/* The cursor readout converts a pixel delta into a sample delta with this,
 * and it is true only because draw_channel_map()
 * plot buf[x] at column x. Decimating or interpolating the plot without
 * changing this would silently rescale every cursor dt. */
_Static_assert(SCOPE_CURSOR_SAMPLES_PER_PIXEL == 1.0f,
               "the plot no longer draws one sample per column — "
               "cursor dt and 1/dt would be wrong");

/*
 * Mask readout: the third slot of badge row 2 (where the legend sits when no
 * mask exists). Below the live band, so the compositor never paints over it,
 * and refreshed on its own epoch from main.c (scope_ui_mask_slot_refresh) --
 * the measurement badges' "stale until the next full repaint" is exactly the
 * defect a pass/fail counter must not have. A SKIP is shown as such: a
 * frozen PASS count over records that are no longer being judged would be
 * the project's signature stable-plausible-wrong number.
 */
#define MASK_SLOT_X ((uint16_t)(2u + 2u * (BADGE_W + 2u)))

static const char *mask_skip_short(mask_pf_reason_t r)
{
    switch (r) {
    case MASK_PF_R_STALE:       return "stale";
    case MASK_PF_R_UNTRIGGERED: return "free-run";
    case MASK_PF_R_NOT_ORDERED: return "no seam";
    case MASK_PF_R_NO_ANCHOR:   return "no trig";
    case MASK_PF_R_SETTINGS:    return "settings";
    case MASK_PF_R_SPAN:        return "span";
    case MASK_PF_R_CLIPPED:     return "clipped";
    default:                    return "?";
    }
}

static void draw_legend_slot(const theme_t *th, bool live)
{
    const uint16_t x = MASK_SLOT_X, y = BADGE_ROW2_Y;
    const char *note = live ? "cnt=ADC raw  --=no tb/cal" : "DEMO trace - no capture";
    if (font_string_width(note, &font_small) > LCD_WIDTH - x)
        note = live ? "--=no tb/cal" : "DEMO trace";
    draw_slot_text(x, y, note, th->text_secondary, th);
}

static void draw_mask_slot(const theme_t *th)
{
    scope_mask_summary_t m;
    scope_mask_summary(&m);
    char b[32];
    uint16_t c = th->text_secondary;
    const uint16_t x = MASK_SLOT_X, y = BADGE_ROW2_Y;

    if (m.state == MASK_PF_TEACHING) {
        if (m.teach_last_reject != MASK_PF_R_OK)
            snprintf(b, sizeof(b), "MASK teach %u/%u %s", (unsigned)m.teach_got,
                     (unsigned)m.teach_target, mask_skip_short(m.teach_last_reject));
        else
            snprintf(b, sizeof(b), "MASK teach %u/%u", (unsigned)m.teach_got,
                     (unsigned)m.teach_target);
        c = th->highlight;
    } else if (m.hold) {
        snprintf(b, sizeof(b), "FAIL %lu/%lu HOLD-OK", (unsigned long)m.failed,
                 (unsigned long)m.tested);
        c = th->warning;
    } else if (m.last.verdict == MASK_PF_V_SKIP) {
        snprintf(b, sizeof(b), "MASK skip: %s", mask_skip_short(m.last.reason));
        c = th->highlight;
    } else if (m.tested == 0u) {
        snprintf(b, sizeof(b), "MASK ready");
        c = th->success;
    } else {
        snprintf(b, sizeof(b), "P%lu F%lu", (unsigned long)m.passed,
                 (unsigned long)m.failed);
        c = (m.last.verdict == MASK_PF_V_FAIL) ? th->warning : th->success;
    }
    draw_slot_text(x, y, b, c, th);
}

void scope_ui_mask_slot_refresh(void)
{
    if (scope_mask_state() == MASK_PF_EMPTY)
        draw_legend_slot(theme_get(), fpga_data_ready());   /* mask just cleared */
    else
        draw_mask_slot(theme_get());
}

/*
 * Measurement badges.
 *
 * Until 2026-08-13 this function printed the string literals "1.00kHz",
 * "707mV", "50.0%" and "1.00ms" — four numbers that had never touched an
 * ADC sample, on a screen a user reads as measurements. They are gone.
 *
 * What is printed now is measured from the live record, in the units this
 * instrument can actually defend (see scope_measure.h):
 *
 *   Vpp / Vrms   VOLTS wherever scope_cal.c has a bench-measured
 *                volts-per-count for that CHANNEL on that range: ranges 4-9,
 *                with 5/6/7 cross-validated three ways and 4/8/9 provisional.
 *                Ranges 0-3 rail on both channels and have no gain at all, so
 *                they still show honest ADC COUNTS ("Ncnt") rather than a
 *                plausible-but-invented voltage.
 *   Duty         a pure ratio; invariant under any affine counts->volts
 *                mapping, so it is already correct and stays correct once
 *                calibration lands. Asserted in tests/test_scope_measure.c.
 *   Freq         Hz from the SPECTRAL estimator (scope_freq.c) on measured
 *                timebase codes; declines rather than guess, with a short
 *                hold (see below). "--" on unmeasured codes.
 *   Per          SECONDS as 1/Freq while the spectral estimate stands;
 *                otherwise SAMPLES between mid-level crossings, never
 *                converted, because that crossing period is 5-50% out
 *                (EXP-13) and a bad number in real units reads as true.
 *
 * The wiring is Vpp = pp * k, Vrms = ac_rms * k, with k from
 * scope_cal_volts_per_count(ch, vdiv_idx), k == 0 meaning "show counts";
 * Hz needs the measured rate table in scope_timebase.c (EXP-17/18) and the
 * reg-0x01 code actually in force. `fpga scope measure` prints these same
 * quantities raw for the bench, from the same sources, so what EXP-19
 * validates is what the screen shows.
 */
static void draw_measurement_badges(const scope_state_t *ss, const theme_t *th)
{
    char buf[16];
    scope_measure_t m1, m2;

    const bool have1 = ss->ch1.enabled &&
                       measure_live_channel(fpga_get_ch1_buf(), &m1);
    const bool have2 = ss->ch2.enabled &&
                       measure_live_channel(fpga_get_ch2_buf(), &m2);
    const uint16_t na = th->text_secondary;   /* dim: nothing to report */

    /* ── Row 1 — CH1 ─────────────────────────────────────────────── */
    uint16_t x = 2;
    uint16_t y1 = BADGE_ROW_Y;

    /*
     * Freq — spectral, not edge-counted, and it declines rather than guess.
     *
     * History, because it is the whole justification for the shape of this:
     * Freq = fs / period_samples was wired up on 2026-08-18 and removed the
     * same evening, measuring +1.6% to +112% against a known drive (EXP-13).
     * The sample rate was never the problem; the rising-crossing period
     * estimator found spurious crossings.
     *
     * EXP-16 then showed it was not the estimator either. Edge counting,
     * autocorrelation and a spectral peak all failed on the SAME bench records
     * and all succeeded on the same others: roughly a fifth to a half of
     * capture records are TORN, with their spectral energy smeared over ~6
     * bins. One record scored 0.380 coherence whole and 0.999 over its last
     * half. Tearing does not track signal amplitude, so it cannot be screened
     * out in advance — it has to be detected per record, which is exactly what
     * scope_freq does. Against 72 bench captures it answers 54% of the time,
     * is within 3.4% when it does, and is never wrong.
     *
     * The reading is HELD across a short run of refusals so the badge does not
     * strobe at the tearing rate; after FREQ_HOLD_FRAMES consecutive refusals
     * it blanks, because a stale number with no expiry is just a wrong number
     * that used to be right.
     */
    static float    freq_hz;
    static uint16_t freq_stale;
    {
        /* timebase_idx IS the reg-0x01 code — fpga_stock_timebase_byte()
         * returns it unchanged, and status_bar.c reads it the same way. */
        const float fs = scope_timebase_sample_rate(ss->timebase_idx);
        scope_freq_t fr;

        /* The acquisition buffer is `volatile` because the acq task fills it;
         * the estimator only reads, and its coherence gate refuses torn
         * records anyway — so an MCU-side tear was never able to produce a
         * WRONG number here, only a refusal. The generation retry (P0.2)
         * recovers those refusals: a commit landing mid-estimate re-runs the
         * estimate on the settled frame instead of burning a hold frame. */
        _Static_assert(FPGA_ADC_BUF_SIZE == SCOPE_FREQ_MAX_N,
                       "estimator window must match the capture buffer");

        bool fe = false;
        if (have1) {
            for (int attempt = 0; attempt < 3; attempt++) {
                uint32_t g0 = fpga_acq_frame_generation();
                fe = scope_freq_estimate((const uint8_t *)fpga_get_ch1_buf(),
                                         FPGA_ADC_BUF_SIZE, fs, &fr);
                if ((g0 & 1u) == 0u && fpga_acq_frame_generation() == g0)
                    break;
            }
        }
        if (fe) {
            freq_hz = fr.hz;
            freq_stale = 0u;
        } else if (freq_stale < FREQ_HOLD_FRAMES) {
            freq_stale++;
        } else {
            freq_hz = 0.0f;
        }

        if (freq_hz > 0.0f) {
            fmt_hz(buf, sizeof(buf), freq_hz);
            draw_one_badge(x, y1, "Freq", buf, th->ch1, th);
        } else {
            draw_one_badge(x, y1, "Freq", MEAS_NA, na, th);
        }
    }
    x += BADGE_W + 2;

    /* Calibrated ranges show volts; all others fall back to ADC counts. */
    const float k1 = scope_cal_volts_per_count(1u, ss->ch1.vdiv_idx);

    if (have1) {
        /* pp_robust, not pp: raw max-min is noise-inflated by +4..+10% of
         * the commanded amplitude (EXP-19) — see scope_measure.h. */
        if (k1 > 0.0f) fmt_volts(buf, sizeof(buf), (float)m1.pp_robust * k1);
        else           fmt_counts(buf, sizeof(buf), m1.pp_robust);
        draw_one_badge(x, y1, "Vpp", buf, th->ch1, th);
    } else {
        draw_one_badge(x, y1, "Vpp", MEAS_NA, na, th);
    }
    x += BADGE_W + 2;

    if (have1) {
        if (k1 > 0.0f) fmt_volts(buf, sizeof(buf), m1.ac_rms * k1);
        else           fmt_counts(buf, sizeof(buf), (unsigned)(m1.ac_rms + 0.5f));
        draw_one_badge(x, y1, "Vrms", buf, th->ch1, th);
    } else {
        draw_one_badge(x, y1, "Vrms", MEAS_NA, na, th);
    }
    x += BADGE_W + 2;

    if (have1 && m1.level_valid) {
        fmt_tenths(buf, sizeof(buf), m1.duty_pct, "%");
        draw_one_badge(x, y1, "Duty", buf, th->ch1, th);
    } else {
        /* Flat/near-flat record: the mid-level threshold would be sitting
         * inside the noise, so a duty figure would be measuring dither. */
        draw_one_badge(x, y1, "Duty", MEAS_NA, na, th);
    }

    /* ── Row 2 — period, CH2, and the legend ─────────────────────── */
    x = 2;
    uint16_t y2 = BADGE_ROW2_Y;

    if (freq_hz > 0.0f) {
        /*
         * SECONDS, derived as 1/Freq from the SPECTRAL estimate above — the
         * one estimator this project trusts (S3: held-out fixture, never
         * wrong on 72 bench records). NOT from period_samples: the
         * mid-level-crossing period is 5-50% out (EXP-13), which is why it
         * was never dressed in real units. Same hold/expiry as the Freq
         * badge, so the two can never disagree on the screen.
         */
        fmt_seconds(buf, sizeof(buf), 1.0f / freq_hz);
        draw_one_badge(x, y2, "Per", buf, th->ch1, th);
    } else if (have1 && m1.period_valid) {
        /* No trustworthy Hz (unmeasured code, or the estimator declined and
         * the hold expired): fall back to the crossing count in honest
         * SAMPLES, never converted (EXP-13). */
        snprintf(buf, sizeof(buf), "%usmp",
                 (unsigned)(m1.period_samples + 0.5f));
        draw_one_badge(x, y2, "Per", buf, th->ch1, th);
    } else {
        draw_one_badge(x, y2, "Per", MEAS_NA, na, th);
    }
    x += BADGE_W + 2;

    if (have2) {
        const float k2 = scope_cal_volts_per_count(2u, ss->ch2.vdiv_idx);
        if (k2 > 0.0f) fmt_volts(buf, sizeof(buf), (float)m2.pp_robust * k2);
        else           fmt_counts(buf, sizeof(buf), m2.pp_robust);
        draw_one_badge(x, y2, "CH2pp", buf, th->ch2, th);
    } else {
        draw_one_badge(x, y2, "CH2pp", MEAS_NA, na, th);
    }
    x += BADGE_W + 2;

    /*
     * The legend is what makes the "--" and the "cnt" suffix readable
     * without the source: it says, on the instrument itself, whether the
     * trace is real and why the missing values are missing.
     */
    (void)x; (void)na;
    if (scope_mask_state() != MASK_PF_EMPTY)
        draw_mask_slot(th);        /* the mask readout takes the legend's slot */
    else
        draw_legend_slot(th, have1 || have2);
}

/* Refresh the badges on their own (main.c), after a volts/div or time/div
 * change: those ride the live compositor now (MOVE Scale, 2026-11-26), and
 * the compositor does not draw the badges. Called once per change, from the
 * first record at the new setting, so this is the same measurement cost the
 * full repaint it replaces paid per press -- NOT per live frame, which once
 * stalled acquisition (see the note at the end of draw_scope_live_frame). */
void scope_ui_badges_refresh(void)
{
    draw_measurement_badges(scope_state_get(), theme_get());
}

/* ═══════════════════════════════════════════════════════════════════
 * Quick-change popup overlay
 * ═══════════════════════════════════════════════════════════════════ */

static void draw_popup_box(const theme_t *th)
{
    (void)th;
    draw_overlay_box();     /* the same box, sized to the text */
    popup_painted = true;
}

static void draw_popup(const theme_t *th)
{
    if (popup_frames == 0) return;
    popup_frames--;
    draw_popup_box(th);
}

/*
 * Popup upkeep for the flicker-free path.
 *
 * Until 2026-08-19 an active popup set `q.force`, which pinned the renderer
 * to the full clear-then-redraw for the popup's whole ~500 ms life — so the
 * cost of showing a two-word box was ten full-screen blanks, and the box
 * itself strobed along with the trace. The popup was not flickering despite
 * the redraw; it was causing it.
 *
 * Now the compositor treats the popup rectangle as a hole (it is the one
 * layer it cannot recompute per pixel, because of the glyphs), paints the box
 * exactly once, and repaints the band underneath on the first frame after the
 * countdown expires. No full redraw is needed at either end.
 */
void scope_popup_overlay_tick(void)
{
    if (popup_frames == 0) return;
    if (!popup_painted)
        draw_popup_box(theme_get());
    popup_frames--;
}

/* ═══════════════════════════════════════════════════════════════════
 * Demo waveform (sine + square)
 * ═══════════════════════════════════════════════════════════════════ */

/* Render one channel's live ADC buffer into the vertical band [y_top, y_bot),
 * AUTO-FITTING the trace to that band from its own min/max (2026-08-14). This
 * makes any signal fill the screen centred, regardless of the frontend gain,
 * the DC offset (DAC1), or amplitude — no clipping, no manual centring. It is
 * a visualisation autoscale, NOT a volts reference; the Vpp/Vrms badges carry
 * the calibrated volts separately. Consecutive samples are connected with a
 * vertical span so the trace reads as a continuous line, not dots. */
/*
 * THE vertical transform. Both renderers go through this.
 *
 * They did not, until 2026-08-19. The full path autoscaled from the buffer's
 * own min/max while the compositor used a fixed (v-128)/256 about mid-screen,
 * over a shorter band, with the channel position offset applied in one and
 * ignored in the other. Same samples, two different places on the glass — so
 * the trace JUMPED whenever the renderer changed, which on the bench looked
 * like "the trace goes back to the bottom after the popup". It hid for a week
 * because a popup or a cursor forced the full path and everything else was
 * the demo waveform.
 *
 * Two copies of a transform that are supposed to agree will not stay agreeing.
 * One copy, two callers.
 */
/* The map itself lives in scope_vmap.c (host-tested), so the zoom cap, the
 * centring and the true-scale geometry are one copy. EXP-77 found the last
 * second copy: the compositor autofitted even with the true-scale graticule
 * on, so the trace jumped scale whenever the renderer changed. */
typedef scope_vmap_t autofit_t;

/* mask_ch: the channel this band draws (0/1), or -1. When a finished mask
 * covers it, the fit also spans the mask's extent, so the bounds drawn over
 * the trace stay on the glass instead of clamping to the band edge. Every
 * caller passes the channel it draws -- trace, compositor, trigger marker --
 * so all three keep agreeing on the scale. */
static void autofit_prep(autofit_t *a, const volatile uint8_t *buf,
                         int16_t y_top, int16_t y_bot, int8_t mask_ch)
{
    uint16_t n = (LCD_WIDTH < 512u) ? (uint16_t)LCD_WIDTH : 512u;
    uint8_t mn = 255, mx = 0;
    for (uint16_t x = 0; x < n; x++) {
        uint8_t sv = buf[x];
        if (sv < mn) mn = sv;
        if (sv > mx) mx = sv;
    }
    uint8_t elo, ehi;
    if (mask_ch >= 0 && scope_mask_extent((uint8_t)mask_ch, &elo, &ehi)) {
        if (elo < mn) mn = elo;
        if (ehi > mx) mx = ehi;
    }
    /* Zoom cap and centring: a quiet record (a grounded probe is +/-1 count
     * of ADC noise) is no longer stretched over the band -- scope_vmap.h. */
    scope_vmap_autofit(a, mn, mx, y_top, y_bot, SCOPE_H);
}

static inline int16_t autofit_y(const autofit_t *a, uint8_t v)
{
    return scope_vmap_y(a, v);
}

/* True scale for this channel: the graticule asks for it and the range has a
 * real volts/div. A NONE range always autofits (its grid has no volts meaning
 * to honour). */
static bool chan_fixed(const scope_state_t *ss, uint8_t ch)
{
    const uint8_t r = (ch == 2u) ? ss->ch2.vdiv_idx : ss->ch1.vdiv_idx;
    return ss->true_scale && scope_cal_true_scale_ok(ch, r);
}

/* A channel's band and map, for EVERY renderer: the full repaint, the live
 * compositor and the trigger marker. `split`: both channels on, CH1 in the
 * top band (0), CH2 in the bottom one (1). Bands end at TRACE_BOT, above the
 * readout rows. True scale keeps 256 counts per SCOPE_H; on its own it is
 * centred on the grid's centre line (where `fpga scope center` parks 0 V),
 * split it is centred in its band. */
/* Each channel's reference row this frame (-1 = not drawn), from its map:
 * where the "1" / "2" marker on the left edge points (scope_vmap_ref_y).
 * Reset with the vertical scales in vband_clear(). */
static int16_t g_chan_ref_y[2] = { -1, -1 };

static void chan_map_prep(autofit_t *a, const volatile uint8_t *buf, bool split,
                          uint8_t band, bool fixed, int8_t mask_ch)
{
    const int16_t top = (split && band) ? (int16_t)(TRACE_MID_Y + 1) : (int16_t)SCOPE_TOP;
    const int16_t bot = (split && !band) ? (int16_t)(TRACE_MID_Y - 1) : (int16_t)TRACE_BOT;
    if (fixed)
        scope_vmap_fixed(a, top, bot,
                         split ? (int16_t)((top + bot) / 2) : (int16_t)SCOPE_MID_Y,
                         128u, SCOPE_H);
    else
        autofit_prep(a, buf, top, bot, mask_ch);
    /* Vertical position (MOVE -> Position, 2026-11-26): `band` is the
     * channel (0 = CH1, 1 = CH2) for every caller. Applied here, in the one
     * map, so the trace, the trigger line and the mask bounds move together
     * in both renderers. */
    const scope_state_t *ss = scope_state_get();
    scope_vmap_offset(a, band ? ss->ch2.position : ss->ch1.position);
    /* The marker points at the channel's 0 V where it is known (EXP-89):
     * the calibrated zero through this same map, so it sits on a grounded
     * trace and clamps to the band edge when 0 V is outside the window.
     * Without a zero (offset never armed) it falls back to the fit's centre. */
    float z;
    g_chan_ref_y[band ? 1 : 0] = scope_chan_zero_counts(band ? 2u : 1u, &z)
                                 ? scope_vmap_y_counts(a, z) : scope_vmap_ref_y(a);
}

/*
 * What the renderer ACTUALLY did to the vertical axis, recorded per band for
 * the cursor readout.
 *
 * The cursor cannot assume the fixed 256/SCOPE_H transform, because the
 * DEFAULT path is autofit and autofit's pixels-per-count is a property of the
 * frame that was drawn — it rescales from the record's own min/max, every
 * frame. Reading the scale off the drawing rather than off an assumption is
 * the same "one copy, two callers" rule that autofit_prep() itself exists to
 * enforce; the two-copies version of that bug (full path vs compositor)
 * already cost this project a week.
 *
 * on == false means "nothing was plotted in this band", which the cursor
 * reports as pixels rather than converting.
 */
typedef struct {
    bool    on;
    int16_t y_top, y_bot;
    float   counts_per_pixel;
    uint8_t channel;            /* 1 or 2 */
    uint8_t range_idx;
} vband_t;

static vband_t g_vband[2];

static void vband_clear(void)
{
    g_vband[0].on = false;
    g_vband[1].on = false;
    g_chan_ref_y[0] = -1;
    g_chan_ref_y[1] = -1;
}

static void vband_note(uint8_t slot, uint8_t channel, uint8_t range_idx,
                       int16_t y_top, int16_t y_bot, float counts_per_pixel)
{
    if (slot > 1u) return;
    g_vband[slot].on               = (counts_per_pixel > 0.0f);
    g_vband[slot].y_top            = y_top;
    g_vband[slot].y_bot            = y_bot;
    g_vband[slot].counts_per_pixel = counts_per_pixel;
    g_vband[slot].channel          = channel;
    g_vband[slot].range_idx        = range_idx;
}

/* autofit_y() maps counts to pixels with h/span, so its inverse — the number
 * of ADC counts one screen row is worth in THIS frame — is span/h. */
static float autofit_counts_per_pixel(const autofit_t *a)
{
    return scope_vmap_counts_per_pixel(a);
}

/*
 * Where the HARDWARE trigger fires, on the glass.
 *
 * The FPGA compares the ADC against reg 0x08 but the record carries ADC - 28,
 * so the crossing that fires sits at (code - 28) in record units: measured on
 * ranges 5 and 7 at six levels (EXP-55/56, median 28) and the same constant
 * as stock's FPGA_ADC_OFFSET. That record code is mapped through the SAME
 * autofit the trigger-source trace is drawn with, so the line sits on the
 * trace where NORMAL would fire. Outside the trace's span it clamps to the
 * band edge (autofit_y's clamp), which on screen reads as "level is above /
 * below the signal" -- exactly the case where NORMAL holds.
 *
 * The code is the one fpga.c records as written (fpga_acq_trig_code_get),
 * not recomputed from the UI level: report the wire, not the intent.
 *
 * The map is the channel's own (chan_map_prep), so this holds in the
 * true-scale view too (EXP-77); a band with no data falls back to the old
 * pixel convention.
 */
static uint16_t scope_soft_trigger_offset(const scope_state_t *ss,
                                          const volatile uint8_t *src_buf);

/* Channel ch's (1/2) range and the code of its offset reference in force:
 * DAC1/PA4 for CH1, TMR13/PA6 for CH2, as the drivers record them. Code 0
 * means "never armed" (emulator, early boot): no zero then. */
static bool chan_offset(uint8_t ch, uint8_t *range, uint16_t *code)
{
    const scope_state_t *ss = scope_state_get();
    *range = (ch == 2u) ? ss->ch2.vdiv_idx : ss->ch1.vdiv_idx;
    *code  = (ch == 2u) ? scope_trigger_ch2_last() : scope_trigger_dac_last();
    return *code != 0u;
}

bool scope_chan_zero_counts(uint8_t ch, float *out)
{
    uint8_t r;
    uint16_t code;
    return chan_offset(ch, &r, &code) && scope_cal_zero_counts(ch, r, code, out);
}

/* The hardware trigger level as volts at the source channel's probe tip:
 * the record code that fires (reg 0x08 - 28, EXP-55/56) less that
 * channel's 0 V, times its volts per count (EXP-89). The 28 was measured on
 * CH1 (ranges 5 and 7 on unit #1) and on both channels of my unit (range 5,
 * EXP-89: the trigger sample within 3 counts of the printed level). */
bool scope_trigger_level_volts(float *out)
{
    const scope_state_t *ss = scope_state_get();
    const uint8_t ch = (ss->trigger.source == TRIG_SRC_CH2) ? 2u : 1u;
    uint8_t r;
    uint16_t code;
    if (!chan_offset(ch, &r, &code))
        return false;
    const float rec = (float)fpga_acq_trig_code_get() + FPGA_ADC_OFFSET;
    return scope_cal_counts_to_volts(ch, r, code, rec, out);
}

static int16_t trigger_marker_y(const scope_state_t *ss, const autofit_t *a)
{
    if (a == NULL || !a->on)
        return (int16_t)(SCOPE_MID_Y - ss->trigger.level);
    int rec = (int)fpga_acq_trig_code_get() + (int)FPGA_ADC_OFFSET;
    if (rec < 0)   rec = 0;
    if (rec > 255) rec = 255;
    return autofit_y(a, (uint8_t)rec);
}

/* Full-path variant: rebuild the trigger source's autofit exactly as
 * draw_demo_waveform() will (same soft-trigger offset, same band split). */
static int16_t trigger_marker_y_full(const scope_state_t *ss)
{
    const volatile uint8_t *b1 = fpga_get_ch1_buf();
    const volatile uint8_t *b2 = fpga_get_ch2_buf();
    if (!fpga_data_ready() || b1 == NULL || b2 == NULL)
        return trigger_marker_y(ss, NULL);
    const bool src2 = (ss->trigger.source == TRIG_SRC_CH2);
    const bool c1 = ss->ch1.enabled, c2 = ss->ch2.enabled;
    const volatile uint8_t *src = src2 ? b2 : b1;
    uint16_t toff = scope_soft_trigger_offset(ss, src);
    autofit_t a = { 0 };
    if ((c1 && c2) || (src2 ? c2 : c1))
        chan_map_prep(&a, src + toff, c1 && c2, src2 ? 1u : 0u,
                      chan_fixed(ss, src2 ? 2u : 1u), src2 ? 1 : 0);
    return trigger_marker_y(ss, &a);
}

/*
 * Mask pass/fail overlay (docs/specs/scope/mask-pass-fail.md).
 *
 * The verdict is computed in the acquisition task (scope_mask.c); this only
 * PAINTS it, per column, from the result already recorded -- no analysis on
 * the display path. Painted only when the window is on the HARDWARE anchor
 * (g_trig_anchor == 2: the alignment the mask was taught on) and only for the
 * frame generation that was actually judged; anything else draws as if there
 * were no mask, rather than colouring a frame the verdict does not describe.
 *
 *   trace   CH1/CH2 trace pixels turn th->warning on columns that were
 *           outside the mask in that record.
 *   strip   3 px at the bottom of the live band over the masked span:
 *           green = passed, red = this column failed, dotted red = the record
 *           failed elsewhere, dotted amber = refused (SKIP), amber = teaching.
 */
static uint8_t  g_trig_anchor = 0;        /* see scope_soft_trigger_offset() */
static int16_t  g_trig_x_actual = -1;
static bool     g_mask_ov_on;
static bool     g_mask_bounds_on;
static uint32_t g_mask_ov_gen;

static void mask_overlay_begin(void)
{
    g_mask_ov_on = (g_trig_anchor == 2u) && g_trig_x_actual >= 0 &&
                   scope_mask_state() != MASK_PF_EMPTY;
    /* The bounds depend on the alignment, not on which record was judged,
     * so they are not gated on the generation: gating them would blink them
     * off for the frame between a commit and its verdict. */
    g_mask_bounds_on = g_mask_ov_on && scope_mask_state() == MASK_PF_READY;
    g_mask_ov_gen = fpga_acq_frame_generation();
}

/* Mask bounds for screen column x of channel ch, as two y values through the
 * band's own transform (yfn). Dotted: every other column. */
#define MASK_BOUND_DOT(x) (((x) & 1u) == 0u)

static inline bool mask_px_fail(int8_t ch, int x)
{
    return g_mask_ov_on && ch >= 0 &&
           scope_mask_column((uint8_t)ch, x, g_trig_x_actual, g_mask_ov_gen) == SCOPE_MASK_COL_FAIL;
}

/* Strip colour for column x, or 0 for none; *dotted = paint every other px. */
static uint16_t mask_strip_color(int x, const theme_t *th, bool *dotted)
{
    *dotted = false;
    if (!g_mask_ov_on) return 0;
    scope_mask_col_t a = scope_mask_column(0, x, g_trig_x_actual, g_mask_ov_gen);
    scope_mask_col_t b = scope_mask_column(1, x, g_trig_x_actual, g_mask_ov_gen);
    scope_mask_col_t k = (a > b) ? a : b;        /* enum order = severity */
    switch (k) {
    case SCOPE_MASK_COL_PASS:       return th->success;
    case SCOPE_MASK_COL_FAIL:       return th->warning;
    case SCOPE_MASK_COL_FAIL_FRAME: *dotted = true; return th->warning;
    case SCOPE_MASK_COL_SKIP:       *dotted = true; return th->highlight;
    case SCOPE_MASK_COL_TEACH:      return th->highlight;
    default:                        return 0;
    }
}

#define MASK_STRIP_H 3u

/* Full-path strip (the compositor paints the same rows per pixel). */
static void draw_mask_strip(const theme_t *th, uint16_t band_bot)
{
    if (!g_mask_ov_on) return;
    for (int x = 0; x < LCD_WIDTH; x++) {
        bool dotted;
        uint16_t c = mask_strip_color(x, th, &dotted);
        if (!c) continue;
        for (uint16_t y = band_bot - MASK_STRIP_H; y < band_bot; y++)
            if (!dotted || ((x + y) & 1u) == 0u)
                lcd_set_pixel((uint16_t)x, y, c);
    }
}

/* Draws one channel through its map (autofit or true scale -- see
 * chan_map_prep) and returns the ADC counts one row was worth in this frame,
 * for the cursor readout. One function for both modes since EXP-77; the
 * true-scale geometry (26 rows per SCOPE_CAL_COUNTS_PER_DIV counts) is pinned
 * by the _Static_assert above and by tests/test_scope_vmap.c. */
static float draw_channel_map(const autofit_t *a, const volatile uint8_t *buf,
                              uint16_t color, int8_t mask_ch)
{
    uint16_t n = (LCD_WIDTH < 512u) ? (uint16_t)LCD_WIDTH : 512u;
    const uint16_t fail_c = theme_get()->warning;
    const uint16_t bnd_c  = theme_get()->text_secondary;
    int16_t prev_y = -1;
    for (uint16_t x = 0; x < n; x++) {
        int16_t y = autofit_y(a, buf[x]);
        uint16_t cc = mask_px_fail(mask_ch, x) ? fail_c : color;
        uint8_t blo, bhi;
        if (g_mask_bounds_on && mask_ch >= 0 && MASK_BOUND_DOT(x) &&
            scope_mask_bound_at((uint8_t)mask_ch, x, g_trig_x_actual, &blo, &bhi)) {
            lcd_set_pixel(x, (uint16_t)autofit_y(a, blo), bnd_c);
            lcd_set_pixel(x, (uint16_t)autofit_y(a, bhi), bnd_c);
        }
        if (prev_y >= 0) {                  /* connect prev..cur vertically */
            int16_t lo = prev_y < y ? prev_y : y;
            int16_t hi = prev_y < y ? y : prev_y;
            for (int16_t v = lo; v <= hi; v++)
                lcd_set_pixel(x, (uint16_t)v, cc);
        } else {
            lcd_set_pixel(x, (uint16_t)y, cc);
        }
        prev_y = y;
    }
    return autofit_counts_per_pixel(a);
}

/* Software display trigger — the fix for the free-running "dancing" trace.
 *
 * Real scopes hold a waveform still by starting every sweep at the same point
 * on the wave: the moment the signal crosses a chosen level going a chosen
 * direction. We do that in software here — find that crossing in the trigger
 * source buffer and return its index, so the render window starts on a
 * repeatable phase. The offset is applied to BOTH channels so they stay
 * time-aligned. It never distorts the samples: it only chooses WHERE the drawn
 * window begins, so amplitude and frequency are untouched.
 *
 * 0 = free-run (disabled, NULL buffer, or no crossing found). "No crossing" is
 * the normal outcome when the level sits outside the signal, and free-running
 * then is exactly AUTO-mode behaviour.
 */
/* Where the trigger point landed on the glass in the last computed window:
 * column 0..319, or -1 when free-running. g_trig_anchor: 0 none, 1 soft
 * (midline crossing), 2 hardware (the level crossing nearest index 512 of a
 * time-ordered record). Read by the position marker and by `spi3 frame`. */
/* g_trig_x_actual / g_trig_anchor are declared with the mask overlay above
 * (it reads them); they start at -1 / 0 via scope_soft_trigger_offset(). */

/* The hardware trigger point of a time-ordered record: the crossing of the
 * level (code - 28, EXP-55/56) in the chosen direction nearest index 512,
 * within +/-48 (EXP-53: 501..520). Schmitt-armed like the soft trigger.
 * Returns the index or -1. */
static int hw_trigger_anchor(const scope_state_t *ss, const volatile uint8_t *buf)
{
    /* Moved to trig_edge_anchor() 2026-10-03 so the mask test aligns on the
     * same crossing the glass does; behaviour unchanged. */
    int c = (int)fpga_acq_trig_code_get() + (int)FPGA_ADC_OFFSET;
    return trig_edge_anchor(buf, c, ss->trigger.edge == TRIG_RISING);
}

static uint16_t scope_soft_trigger_offset(const scope_state_t *ss,
                                          const volatile uint8_t *src_buf)
{
    g_trig_x_actual = -1;
    g_trig_anchor = 0;
    if (!ss->soft_trigger || src_buf == NULL)
        return 0;

    uint16_t draw_n = (LCD_WIDTH < 512u) ? (uint16_t)LCD_WIDTH : 512u;
    if (FPGA_ADC_BUF_SIZE <= draw_n)
        return 0;                                 /* no slack to slide */
    uint16_t max_start = (uint16_t)(FPGA_ADC_BUF_SIZE - draw_n);

    /* Horizontal position: the column the trigger point should land on. */
    int tx = ss->trig_x;
    if (tx < 8) tx = 8;
    if (tx > (int)draw_n - 8) tx = (int)draw_n - 8;

    /* 1. Hardware anchor (2026-09-22): a time-ordered record carries the
     *    hardware trigger at index 512, and the crossing nearest it IS the
     *    event that fired. CH1 only -- the level register was measured there.
     *    Tested on the trigger SOURCE, not the buffer address: `spi3 frame`
     *    passes a snapshot copy, and an address test made the bench check
     *    exercise the fallback while the screen used the anchor (v17). */
    if (ss->trigger.source != TRIG_SRC_CH2 && fpga_acq_record_time_ordered()) {
        int a = hw_trigger_anchor(ss, src_buf);
        if (a >= 0) {
            int start = a - tx;
            if (start < 0) start = 0;
            if (start > (int)max_start) start = (int)max_start;
            g_trig_x_actual = (int16_t)(a - start);
            g_trig_anchor = 2;
            return (uint16_t)start;
        }
    }

    /* SEAM GUARD (EXP-22, 2026-09-03): the acquisition record is NOT always
     * time-contiguous at its edges. Saved-frame analysis found ~9-14-sample
     * phase discontinuities clustered at indices 32..96 (a stale head —
     * residue roughly one read cadence old, the same defect family as the
     * "reads are off by one byte" finding) and 864..928. A trigger crossing
     * fabricated at such a seam locks the trace to an arbitrary phase — at
     * 500 Hz it showed as frames flipping 180 degrees, ~11-sample p95 jitter,
     * indistinguishable from free-run. Every frame whose seam fell OUTSIDE
     * the drawn window locked to <0.1 sample, so the algorithm is sound and
     * the guard just keeps it off the bad data: start the search at
     * SEAM_GUARD, which also keeps the window [off, off+draw_n] clear of the
     * late seams for any signal above ~35 Hz at the slow timebases. The
     * underlying acquisition defect is filed, not fixed, by this.
     * The number itself lives in scope_record.h so the spectrum views skip
     * the same head (2026-09-15). */
    const uint16_t SEAM_GUARD = SCOPE_RECORD_HEAD_SKIP;
    if (max_start <= SEAM_GUARD)
        return 0;

    /* Auto-level: base the threshold at the SIGNAL'S OWN midline, not a fixed
     * ADC 128. The capture is frequently not centred (e.g. mean 84, range
     * 34..136 on the bench), so a fixed 128 sits up near the peak where the
     * slope is shallow and the crossing jitters frame to frame — which reads on
     * screen as the trace dancing. The 50% level (min+max)/2 is the steep
     * mid-slope crossing and locks cleanly regardless of DC offset;
     * trigger.level then shifts it up/down from there (true-scale slope:
     * SCOPE_H px == 256 counts). Min/max over the guarded region only, so a
     * seam's glitch cannot skew the threshold either. */
    uint8_t mn = 255, mx = 0;
    for (uint16_t i = SEAM_GUARD; i < FPGA_ADC_BUF_SIZE; i++) {
        uint8_t s = src_buf[i];
        if (s < mn) mn = s;
        if (s > mx) mx = s;
    }
    if ((int)mx - (int)mn < 8)
        return 0;                                 /* ~flat: nothing to lock to */

    int thr = ((int)mn + (int)mx) / 2 + ((int)ss->trigger.level * 256) / SCOPE_H;
    if (thr < (int)mn + 1) thr = (int)mn + 1;     /* keep it inside the signal */
    if (thr > (int)mx - 1) thr = (int)mx - 1;

    /* 2. Soft anchor: the midline crossing, searched so that the window
     *    [crossing - tx, crossing - tx + draw_n) stays inside the record and
     *    clear of the guarded head. */
    int idx = scope_measure_find_trigger((const uint8_t *)src_buf + SEAM_GUARD + tx,
                                         (uint16_t)(max_start - SEAM_GUARD),
                                         (uint8_t)thr,
                                         ss->trigger.edge == TRIG_RISING, 3u);
    if (idx <= 0)
        return 0u;
    int a = idx + SEAM_GUARD + tx;
    g_trig_x_actual = (int16_t)tx;
    g_trig_anchor = 1;
    return (uint16_t)(a - tx);
}

int16_t scope_ui_trig_x_actual(void) { return g_trig_x_actual; }
uint8_t scope_ui_trig_anchor(void)   { return g_trig_anchor; }

/* Exported for bench validation (`spi3 frame`): the offset the renderer would
 * start the drawn window at for the CURRENT scope state. Exporting the real
 * function rather than letting the host re-implement it is deliberate — a
 * host replica passing while the on-screen algorithm fails is exactly the
 * class of stable-wrong-number this project keeps paying for. */
uint16_t scope_ui_soft_trigger_offset(const volatile uint8_t *src_buf)
{
    return scope_soft_trigger_offset(scope_state_get(), src_buf);
}

void draw_demo_waveform(uint32_t frame)
{
    const theme_t *th = theme_get();
    const scope_state_t *ss = scope_state_get();

    /* Forget last frame's vertical scales before anything is drawn. A band
     * that is not plotted this frame — no capture, channel off, or the demo
     * waveform below, whose amplitude is decorative — must leave the cursor
     * with NO transform, so it reports pixels instead of converting through a
     * scale that belongs to a trace no longer on the screen. */
    vband_clear();
    g_mask_ov_on = false;          /* re-armed by mask_overlay_begin() on live data */

    /* Freeze waveform when stopped — hold the last running frame */
    static uint32_t frozen_frame = 0;
    if (ss->running) {
        frozen_frame = frame;
    }
    uint32_t draw_frame = ss->running ? frame : frozen_frame;

    /*
     * If FPGA has delivered real ADC data, render from the sample buffers.
     * Each buffer holds 512 samples (normal mode) as unsigned 8-bit values
     * centered around 128 (after ADC offset calibration).
     * We map the 0-255 range into the waveform display area.
     */
    if (fpga_data_ready()) {
        /* Latch: once we've ever seen real ADC data, disable the
         * synthetic demo fallback permanently. */
        scope_mark_acquisition_ready();

        const volatile uint8_t *ch1_buf = fpga_get_ch1_buf();
        const volatile uint8_t *ch2_buf = fpga_get_ch2_buf();

        /* Re-triggering DISABLED — see note in demo waveform fallback.
         * TODO: re-enable once SPI3 protocol is confirmed working. */

        /* Auto-fit render. If both channels are live, split the scope area
         * into top (CH1) and bottom (CH2) bands so the traces don't overlap;
         * a single live channel gets the whole area. */
        bool c1 = ss->ch1.enabled && ch1_buf != NULL;
        bool c2 = ss->ch2.enabled && ch2_buf != NULL;

        /* Software trigger: find the phase on the trigger SOURCE channel and
         * shift both channels' render windows by the same offset, so the trace
         * stops free-running (dancing) while staying time-aligned. */
        const volatile uint8_t *trig_src =
            (ss->trigger.source == TRIG_SRC_CH2 && ch2_buf) ? ch2_buf :
            (ch1_buf ? ch1_buf : ch2_buf);
        uint16_t toff = scope_soft_trigger_offset(ss, trig_src);
        mask_overlay_begin();
        const volatile uint8_t *b1 = (ch1_buf != NULL) ? ch1_buf + toff : NULL;
        const volatile uint8_t *b2 = (ch2_buf != NULL) ? ch2_buf + toff : NULL;

        /* Each channel through its own map (chan_map_prep: the same one the
         * live compositor and the trigger marker use), and whatever transform
         * it was drawn with is recorded for the cursor readout -- vband_note().
         * Default ss->true_scale == false keeps every build on autofit unless
         * the graticule is switched. */
        const bool split = c1 && c2;
        if (c1) {
            autofit_t m;
            chan_map_prep(&m, b1, split, 0u, chan_fixed(ss, 1u), 0);
            vband_note(0u, 1u, ss->ch1.vdiv_idx, m.y_top, m.y_bot,
                       draw_channel_map(&m, b1, th->ch1, 0));
        }
        if (c2) {
            autofit_t m;
            chan_map_prep(&m, b2, split, 1u, chan_fixed(ss, 2u), 1);
            vband_note(split ? 1u : 0u, 2u, ss->ch2.vdiv_idx, m.y_top, m.y_bot,
                       draw_channel_map(&m, b2, th->ch2, 1));
        }
        return;
    }

    /* ── Fallback: synthetic demo waveform (no FPGA data yet) ──────
     *
     * Only rendered if acquisition has NEVER produced real data. Once
     * the latch flips, we leave the grid empty instead — otherwise a
     * transient FPGA stall would flash fake waveforms on top of a
     * stopped trace, which is worse than an empty screen. */
    if (scope_acquisition_ready()) {
        return;
    }

    /* SPI3 acquisition triggers are now fired from main.c display loop
     * with 500ms warmup delay and early-abort safety. See main.c and
     * fpga_acquisition_task() for the crash-protection logic. */

    /* Mark the trace as synthetic ON the trace. The badge legend says the
     * same thing at the bottom of the screen, but the waveform is what
     * catches the eye and it is the thing that is not real. */
    font_draw_string(4, SCOPE_TOP + 14, "DEMO", th->warning, th->warning,
                     &font_small);

    static const int8_t sin_lut[64] = {
         0, 10, 19, 29, 38, 47, 56, 63, 71, 77, 83, 88, 92, 96, 98, 99,
        100, 99, 98, 96, 92, 88, 83, 77, 71, 63, 56, 47, 38, 29, 19, 10,
         0,-10,-19,-29,-38,-47,-56,-63,-71,-77,-83,-88,-92,-96,-98,-99,
       -100,-99,-98,-96,-92,-88,-83,-77,-71,-63,-56,-47,-38,-29,-19,-10,
    };

    /* CH1: sine wave — scale based on vdiv index */
    if (ss->ch1.enabled) {
        int16_t ch1_center = SCOPE_MID_Y - ss->ch1.position;
        int16_t amplitude = 40 - (int16_t)(ss->ch1.vdiv_idx * 2);
        if (amplitude < 10) amplitude = 10;

        for (uint16_t x = 0; x < LCD_WIDTH; x++) {
            uint8_t idx = (uint8_t)((x * 4 + draw_frame) & 0x3F);
            int16_t y = ch1_center - (sin_lut[idx] * amplitude / 100);
            if (y >= SCOPE_TOP && y < SCOPE_BOT) {
                lcd_set_pixel(x, (uint16_t)y, th->ch1);
                if (y + 1 < SCOPE_BOT)
                    lcd_set_pixel(x, (uint16_t)(y + 1), th->ch1);
            }
        }
    }

    /* CH2: square wave */
    if (ss->ch2.enabled) {
        int16_t ch2_center = SCOPE_MID_Y + 50 - ss->ch2.position;

        for (uint16_t x = 0; x < LCD_WIDTH; x++) {
            uint8_t phase = (uint8_t)((x * 4 + draw_frame) & 0x3F);
            int16_t y = ch2_center + (phase < 32 ? -25 : 25);
            if (y >= SCOPE_TOP && y < SCOPE_BOT)
                lcd_set_pixel(x, (uint16_t)y, th->ch2);

            /* Vertical edges */
            if (x > 0) {
                uint8_t prev = (uint8_t)(((x - 1) * 4 + draw_frame) & 0x3F);
                if ((prev < 32) != (phase < 32)) {
                    int16_t y1 = ch2_center - 25;
                    int16_t y2 = ch2_center + 25;
                    for (int16_t yy = y1; yy <= y2; yy++) {
                        if (yy >= SCOPE_TOP && yy < SCOPE_BOT)
                            lcd_set_pixel(x, (uint16_t)yy, th->ch2);
                    }
                }
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Cursor measurement drawing
 * ═══════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════
 * Cursors
 *
 * The lines are a pixel test shared by both renderers (cursor_px), so the
 * live compositor draws exactly what the full path draws. Their labels and
 * the readout are chips (below). Lines stay inside the trace band
 * [SCOPE_TOP, TRACE_BOT): below it are the badge rows, which the compositor
 * does not touch, so a line there would be drawn by one renderer only.
 * ═══════════════════════════════════════════════════════════════════ */

/* Longest chip text: the dt readout is two 23-character readings. */
#define CHIP_TEXT 64

/* Does a cursor line own pixel (x, y), and in which colour? The active cursor
 * is solid in the highlight colour, the others dashed (4 on, 4 off) in the
 * secondary text colour. Checked H2, H1, V2, V1: the order the lines used to
 * be painted in, later over earlier. */
static bool cursor_px(const cursor_state_t *c, const theme_t *th,
                      uint16_t x, uint16_t y, uint16_t *color)
{
    if (c->mode == CURSOR_OFF) return false;
    if (y < SCOPE_TOP || y >= TRACE_BOT) return false;

    if (c->mode == CURSOR_HORIZONTAL || c->mode == CURSOR_BOTH) {
        const bool a2 = (c->active == CURSOR_SEL_H2);
        const bool a1 = (c->active == CURSOR_SEL_H1);
        if (y == c->h2_y && (a2 || (x & 7u) < 4u)) {
            *color = a2 ? th->highlight : th->text_secondary;
            return true;
        }
        if (y == c->h1_y && (a1 || (x & 7u) < 4u)) {
            *color = a1 ? th->highlight : th->text_secondary;
            return true;
        }
    }
    if (c->mode == CURSOR_VERTICAL || c->mode == CURSOR_BOTH) {
        const bool a2 = (c->active == CURSOR_SEL_V2);
        const bool a1 = (c->active == CURSOR_SEL_V1);
        if (x == c->v2_x && (a2 || (y & 7u) < 4u)) {
            *color = a2 ? th->highlight : th->text_secondary;
            return true;
        }
        if (x == c->v1_x && (a1 || (y & 7u) < 4u)) {
            *color = a1 ? th->highlight : th->text_secondary;
            return true;
        }
    }
    return false;
}

/* Full path: walk the two columns and two rows the lines can occupy. */
static void draw_cursors(const scope_state_t *ss, const theme_t *th)
{
    const cursor_state_t *c = &ss->cursor;
    if (c->mode == CURSOR_OFF) return;

    uint16_t col;
    const uint16_t xs[2] = { c->v1_x, c->v2_x };
    for (uint8_t i = 0; i < 2u; i++) {
        if (xs[i] >= LCD_WIDTH) continue;
        for (uint16_t y = SCOPE_TOP; y < TRACE_BOT; y++)
            if (cursor_px(c, th, xs[i], y, &col))
                lcd_set_pixel(xs[i], y, col);
    }
    const uint16_t ys[2] = { c->h1_y, c->h2_y };
    for (uint8_t i = 0; i < 2u; i++) {
        if (ys[i] < SCOPE_TOP || ys[i] >= TRACE_BOT) continue;
        for (uint16_t x = 0; x < LCD_WIDTH; x++)
            if (cursor_px(c, th, x, ys[i], &col))
                lcd_set_pixel(x, ys[i], col);
    }
}

/* The cursor readout as text, one line per axis; returns the line count.
 * Reads the vertical scale the renderer recorded THIS frame (g_vband), so
 * both renderers must note their bands before building chips. */
static uint8_t cursor_readout(const scope_state_t *ss, char lines[2][CHIP_TEXT])
{
    const cursor_state_t *c = &ss->cursor;
    uint8_t n = 0;
    char buf[24], buf2[24];

    /*
     * Delta readout.
     *
     * Nothing here converts anything itself. The pixel deltas are turned into
     * real units by scope_cursor.c, which reads the SAME measured tables the
     * status bar and the measurement badges read — scope_timebase.c for the
     * sample rate and scope_cal.c for the per-(channel,range) gain — and
     * refuses to name a unit when the table has no entry.
     *
     * What the axes are worth, and why each is exact:
     *
     *   horizontal — the plot draws buf[x] at column x, so dx pixels IS dx
     *                samples (asserted at the top of this file). Seconds need
     *                the reg-0x01 rate; without one the readout stays in
     *                samples.
     *   vertical   — pixels-per-count is whatever the renderer used for the
     *                band the cursors are in, recorded by vband_note() as it
     *                draws. It is NOT a constant: autofit, the default path,
     *                rescales from each record's own min/max. Volts then need
     *                the range's gain; without one the readout stays in
     *                counts, and without even a transform it stays in pixels.
     *
     * Until 2026-09-12 this read two floats in cursor_state_t that nothing
     * ever updated: 10 ms across the screen and 8 V down it, seeded once at
     * init. Every dt and dV the instrument had ever printed came from those
     * two numbers. They were zeroed in August, which made the readout honest
     * but left it disconnected from the tables that had just been measured.
     * The floats are gone; there is no second copy of either constant now.
     */
    if (c->mode == CURSOR_VERTICAL || c->mode == CURSOR_BOTH) {
        const int32_t dx = (int32_t)c->v2_x - (int32_t)c->v1_x;

        /* ss->timebase_idx IS the reg-0x01 code: the same field the status
         * bar labels and the Freq badge derive from, so the three cannot
         * disagree about what rate is in force. */
        scope_cursor_reading_t dt = scope_cursor_delta_t(ss->timebase_idx, dx);
        scope_cursor_format(&dt, buf, sizeof(buf));
        scope_cursor_reading_t f = scope_cursor_one_over_dt(ss->timebase_idx, dx);
        scope_cursor_format(&f, buf2, sizeof(buf2));
        snprintf(lines[n++], CHIP_TEXT, "dt=%s  1/dt=%s", buf, buf2);
    }

    if (c->mode == CURSOR_HORIZONTAL || c->mode == CURSOR_BOTH) {
        /* Second cursor minus first, the same convention dt uses (v2 - v1).
         * Screen y grows downward while volts grow upward, so H2 minus H1 in
         * volts is h1_y minus h2_y in pixels. */
        const int32_t dy = (int32_t)c->h1_y - (int32_t)c->h2_y;

        /*
         * Only a band that contains BOTH cursors can convert this. In split
         * (two-channel) mode each band has its own autofit scale, so a delta
         * spanning the two is not a voltage in either channel; it falls back
         * to pixels rather than picking one scale and hoping.
         */
        scope_cursor_vmap_t vm = { 0.0f, 0u, 0u };
        for (uint8_t i = 0u; i < 2u; i++) {
            const vband_t *b = &g_vband[i];
            if (!b->on) continue;
            if ((int16_t)c->h1_y < b->y_top || (int16_t)c->h1_y > b->y_bot) continue;
            if ((int16_t)c->h2_y < b->y_top || (int16_t)c->h2_y > b->y_bot) continue;
            vm.counts_per_pixel = b->counts_per_pixel;
            vm.channel          = b->channel;
            vm.range_idx        = b->range_idx;
            break;
        }

        scope_cursor_reading_t dv = scope_cursor_delta_v(&vm, dy);
        scope_cursor_format(&dv, buf, sizeof(buf));

        /* Name the channel when the number is a voltage: with two channels on
         * two ranges, "dV=1.20V" alone does not say which frontend measured
         * it. */
        if (dv.unit == SCOPE_CURSOR_UNIT_VOLTS)
            snprintf(lines[n++], CHIP_TEXT, "dV%u=%s", vm.channel, buf);
        else
            snprintf(lines[n++], CHIP_TEXT, "dV=%s", buf);
    }
    return n;
}

/* ═══════════════════════════════════════════════════════════════════
 * Band chips
 *
 * Every piece of text that sits on the trace band is a chip: an opaque box
 * of font_small text, painted with font_draw_string_box (every pixel written
 * exactly once, nothing blanked first). The full path paints the chips on top
 * of everything except the popup; the live compositor cuts them out of its
 * column stream as holes (band_holes.h) and paints them after it, so the two
 * renderers produce the same pixels and neither ever shows a blank state.
 *
 * Before this (2026-11-26) the live compositor had no holes but the popup:
 * the top row (channel label, SPI3 counter, trigger status, STOP) was erased
 * by the first live frame after every full repaint, and the cursor labels and
 * readout were full-path only, which is why any cursor forced the scope back
 * onto clear-and-repaint (the TRIGGER-button blink, EXP-85). The readout also
 * sat on the badge rows (SCOPE_BOT - 28) and the badges painted over it.
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    band_rect_t r;          /* the hole: font height + 2 * pad tall */
    uint16_t    fg, bg;
    uint8_t     align;      /* FONT_ALIGN_* */
    uint8_t     pad;        /* rows of bg above and below the text */
    char        text[CHIP_TEXT];
} band_chip_t;

#define CHIP_MAX  17u       /* holes = chips + the popup <= BAND_MAX_HOLES */
_Static_assert(CHIP_MAX + 1u <= BAND_MAX_HOLES, "chips + popup must fit the holes");
static band_chip_t g_chip[CHIP_MAX];
static uint8_t     g_nchip;

/* What is on the glass: the chips the last paint left there. The column
 * stream never writes inside a hole, so a chip whose rectangle, colours and
 * text are unchanged is still correct and the live path skips it. Painting
 * all of them every live frame cost ~12 % of the frame rate on my unit
 * (86 -> 76 live frames per 10 s). Only the full path (which repaints every
 * chip after its clear) and a live repaint of the chip itself refresh this. */
static band_chip_t g_chip_shown[CHIP_MAX];
static uint8_t     g_nchip_shown;

/* Channel reference marker: the chip's width and its pixel tip, 3 columns
 * right of the chip pointing at the trace. Set by scope_chips_build. */
#define REF_CHIP_W  9
static struct { bool on; int16_t yc; uint16_t color; } g_ref_tip[2];

static inline bool chan_ref_px(int x, int y, uint16_t *c)
{
    const int dx = x - REF_CHIP_W;
    if (dx < 0 || dx > 2) return false;
    for (int i = 1; i >= 0; i--) {        /* CH2 over CH1, as the traces */
        if (!g_ref_tip[i].on) continue;
        int dy = y - g_ref_tip[i].yc;
        if (dy < 0) dy = -dy;
        if (dy <= 2 - dx) { *c = g_ref_tip[i].color; return true; }
    }
    return false;
}

static void chip_add(int x, int y, int w, uint8_t pad, const char *text,
                     uint16_t fg, uint16_t bg, uint8_t align)
{
    if (g_nchip >= CHIP_MAX || w <= 0) return;
    if (x < 0) x = 0;
    if (x + w > LCD_WIDTH) w = LCD_WIDTH - x;
    if (w <= 0) return;
    band_chip_t *k = &g_chip[g_nchip++];
    memset(k, 0, sizeof(*k));           /* memcmp in scope_chips_paint */
    k->r.x = (uint16_t)x;
    k->r.y = (uint16_t)y;
    k->r.w = (uint16_t)w;
    k->r.h = (uint16_t)(font_small.height + 2u * pad);
    k->fg = fg;
    k->bg = bg;
    k->align = align;
    k->pad = pad;
    snprintf(k->text, sizeof(k->text), "%s", text);
}

static void chip_paint(const band_chip_t *k)
{
    const font_t *f = &font_small;
    if (k->pad)
        lcd_fill_rect(k->r.x, k->r.y, k->r.w, k->pad, k->bg);
    font_draw_string_box(k->r.x, (uint16_t)(k->r.y + k->pad), k->r.w, k->text,
                         k->fg, k->bg, f, k->align);
    if (k->pad)
        lcd_fill_rect(k->r.x, (uint16_t)(k->r.y + k->pad + f->height),
                      k->r.w, k->pad, k->bg);
}

static bool rect_overlap(const band_rect_t *a, const band_rect_t *b)
{
    return a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}

/* Build this frame's chips from the state. Call after the traces have noted
 * their bands (vband_note): the dV readout reads them. */
static void scope_chips_build(const scope_state_t *ss, const theme_t *th)
{
    const uint16_t fh   = font_small.height;
    /* The top row starts on the band's first row, one row of padding above
     * and below the text. Flush with the top, a column under a chip streams
     * as ONE segment below it; two rows lower, every such column paid a
     * second lcd_set_window, ~5 % of the live frame rate on my unit. */
    const int      row0 = SCOPE_TOP;
    g_nchip = 0;

    /* Top row, left to right: active channel, SPI3 counter (Debug Info
     * only), STOP, trigger status. Fixed widths, so a shorter text never
     * leaves a tail behind. */
    chip_add(4, row0, 28, 1, (active_channel == 0) ? "CH1" : "CH2",
             (active_channel == 0) ? th->ch1 : th->ch2, th->background,
             FONT_ALIGN_LEFT);

#ifndef EMULATOR_BUILD
    if (ss->debug_info) {
        /* SPI3 acquisition diagnostic: the FPGA data flow without serial.
         * Behind Settings > Oscilloscope > Debug Info since 2026-11-26: a
         * developer readout from the August 2026 bring-up, not a reading. */
        char b[24];
        uint16_t col;
        if (fpga.spi3_ok_count > 0) {
            snprintf(b, sizeof(b), "SPI3:OK %u", (unsigned)fpga.spi3_ok_count);
            col = th->success;
        } else if (fpga.spi3_total_timeouts > 0) {
            snprintf(b, sizeof(b), "SPI3:-- %u [%02X]",
                     (unsigned)fpga.spi3_total_timeouts, fpga.spi3_first_byte);
            col = th->warning;
        } else if (fpga.initialized) {
            snprintf(b, sizeof(b), "SPI3:wait");
            col = th->text_secondary;
        } else {
            snprintf(b, sizeof(b), "SPI3:off");
            col = th->text_secondary;
        }
        /* Sized to the text: an opaque chip hides the trace under it. */
        chip_add(36, row0, font_string_width(b, &font_small) + 2, 1, b, col,
                 th->background, FONT_ALIGN_LEFT);
    }
#endif

    if (!ss->running)
        chip_add((LCD_WIDTH - 40) / 2, row0, 40, 2, "STOP",
                 th->text_primary, th->warning, FONT_ALIGN_CENTER);

    {
        const char *label;
        uint16_t color;
        if (!ss->running) {
            label = "STOP";   color = th->warning;
        } else if (ss->trigger.mode == TRIG_AUTO) {
            label = "Auto";   color = th->success;
        } else if (ss->trigger.mode == TRIG_SINGLE) {
            label = "Ready";  color = th->highlight;
        } else {
            label = "Trig'd"; color = th->success;
        }
        chip_add(LCD_WIDTH - 46, row0, 44, 1, label, color, th->background,
                 FONT_ALIGN_RIGHT);
    }

    /* What the arrow keys do (MOVE, 2026-11-26): a tag left of the trigger
     * status, in the colour of what it moves. While the trigger panel is
     * open the panel has the keys and says so itself. */
    if (!scope_trig_menu.open) {
        const move_mode_t mm = scope_move_mode_now();
        const char *t = move_mode_name(mm);
        uint16_t bg;
        switch (mm) {
        case MOVE_TRIGGER:  bg = th->trigger;   break;
        case MOVE_POSITION: bg = (active_channel == 0) ? th->ch1 : th->ch2; break;
        case MOVE_MASK:     bg = th->warning;   break;
        case MOVE_CURSORS:  bg = th->highlight; break;
        default:            bg = th->text_secondary; break;
        }
        /* Fixed width (the longest tag, "Position"), so a shorter tag never
         * leaves a tail and the hole does not move. */
        const int w = font_string_width("Position", &font_small) + 8;
        chip_add(LCD_WIDTH - 48 - w, row0, w, 1, t, th->background, bg,
                 FONT_ALIGN_CENTER);
    }

    /* Channel reference markers on the left edge (2026-11-26): a "1" / "2"
     * flag in the channel colour at the channel's 0 V (EXP-89; the fit's
     * centre where no zero is known), moving with the vertical position and
     * clamped to the band edge when 0 V is off the window. Kept clear of the
     * top row and the readout rows; the tip is pixel-drawn (chan_ref_px). */
    for (uint8_t c = 0; c < 2u; c++) {
        g_ref_tip[c].on = false;
        if (g_chan_ref_y[c] < 0) continue;
        const int h = fh + 2;
        int y = g_chan_ref_y[c] - h / 2;
        if (y < row0 + h + 1) y = row0 + h + 1;
        if (y > TRACE_BOT - h) y = TRACE_BOT - h;
        const uint16_t col = c ? th->ch2 : th->ch1;
        chip_add(0, y, REF_CHIP_W, 1, c ? "2" : "1", th->background, col,
                 FONT_ALIGN_CENTER);
        g_ref_tip[c].on = true;
        g_ref_tip[c].yc = (int16_t)(y + h / 2);
        g_ref_tip[c].color = col;
    }

    /* Cursor readout under the top row, and a label beside each line. */
    const cursor_state_t *c = &ss->cursor;
    if (c->mode != CURSOR_OFF) {
        char lines[2][CHIP_TEXT];
        const uint8_t n = cursor_readout(ss, lines);
        int y = row0 + fh + 2 + 2;          /* below the top row (fh + 2 tall) */
        for (uint8_t i = 0; i < n; i++, y += fh)
            chip_add(4, y, font_string_width(lines[i], &font_small) + 4, 0,
                     lines[i], th->highlight, th->background, FONT_ALIGN_LEFT);

        if (c->mode == CURSOR_VERTICAL || c->mode == CURSOR_BOTH) {
            /* Near the bottom of the band, right of the line (left of it near
             * the right edge), clear of the top row's labels. Three rows up
             * from the band's end, so a trace on the bottom edge (autofit
             * puts the record's minimum there) still shows under them. */
            const struct { uint16_t x; bool act; const char *t; } v[2] = {
                { c->v1_x, c->active == CURSOR_SEL_V1, "V1" },
                { c->v2_x, c->active == CURSOR_SEL_V2, "V2" },
            };
            for (uint8_t i = 0; i < 2u; i++) {
                const int w = font_string_width(v[i].t, &font_small) + 2;
                int x = v[i].x + 2;
                if (x + w > LCD_WIDTH) x = (int)v[i].x - 1 - w;
                chip_add(x, TRACE_BOT - fh - 3, w, 0, v[i].t,
                         v[i].act ? th->highlight : th->text_secondary,
                         th->background, FONT_ALIGN_LEFT);
            }
        }
        if (c->mode == CURSOR_HORIZONTAL || c->mode == CURSOR_BOTH) {
            /* Above the line (below it near the top), left of the trigger
             * arrow at the right edge. */
            const struct { uint16_t y; bool act; const char *t; } h[2] = {
                { c->h1_y, c->active == CURSOR_SEL_H1, "H1" },
                { c->h2_y, c->active == CURSOR_SEL_H2, "H2" },
            };
            for (uint8_t i = 0; i < 2u; i++) {
                const int w = font_string_width(h[i].t, &font_small) + 2;
                int y = (h[i].y >= SCOPE_TOP + fh + 1) ? (int)h[i].y - fh - 1
                                                       : (int)h[i].y + 2;
                if (y + fh > TRACE_BOT) y = TRACE_BOT - fh;
                chip_add(LCD_WIDTH - 10 - w, y, w, 0, h[i].t,
                         h[i].act ? th->highlight : th->text_secondary,
                         th->background, FONT_ALIGN_LEFT);
            }
        }
    }

    /* The trigger panel (EXP-86), top right under the status row: a title,
     * one row per setting with the selected one inverted, and a hint. Chips
     * like everything else here, so it opens, changes and closes without a
     * blank frame. Added last: it is on top of the cursor labels. */
    if (scope_trig_menu.open) {
        const int pw = 112;
        const int px = LCD_WIDTH - 4 - pw;
        int py = row0 + fh + 2 + 2;
        chip_add(px, py, pw, 1, "Trigger", th->background, th->highlight,
                 FONT_ALIGN_CENTER);
        py += fh + 2;
        for (uint8_t r = 0; r < TM_ROW_COUNT; r++, py += fh + 2) {
            char t[32];
            snprintf(t, sizeof(t), " %s %s", trig_menu_row_label(r),
                     trig_menu_row_value(ss, r));
            const bool sel = (scope_trig_menu.row == r);
            chip_add(px, py, pw, 1, t,
                     sel ? th->background : th->text_primary,
                     sel ? th->text_primary : th->background, FONT_ALIGN_LEFT);
        }
        chip_add(px, py, pw, 1, " LT/RT change", th->text_secondary,
                 th->background, FONT_ALIGN_LEFT);
    }

    /* While the popup is up, a chip under it is dropped, so the popup is
     * the only thing in its rectangle and the stream repaints the rest. */
    if (popup_frames > 0) {
        const band_rect_t p = popup_rect();
        uint8_t k = 0;
        for (uint8_t i = 0; i < g_nchip; i++)
            if (!rect_overlap(&g_chip[i].r, &p))
                g_chip[k++] = g_chip[i];
        g_nchip = k;
    }
}

/* Full path: after its clear every chip must be painted. */
static void scope_chips_paint(void)
{
    for (uint8_t i = 0; i < g_nchip; i++)
        chip_paint(&g_chip[i]);
    memcpy(g_chip_shown, g_chip, sizeof(g_chip));
    g_nchip_shown = g_nchip;
}

/* Live path: paint only what differs from what is shown. A chip that
 * appeared, moved, or changed text or colour is repainted. One that went away
 * needs nothing itself: its rectangle is no longer a hole and the stream has
 * just repainted it. Overlaps are the catch: a chip under a changed or
 * vanished one, or under one being repainted, holds pixels that are not its
 * own, so any chip overlapping something dirty is dirty too, and dirty chips
 * are painted in list order (the full path's z-order). */
static bool chip_shown(const band_chip_t *k, const band_chip_t *set, uint8_t n)
{
    for (uint8_t j = 0; j < n; j++)
        if (memcmp(k, &set[j], sizeof(band_chip_t)) == 0)
            return true;
    return false;
}

static void scope_chips_paint_changed(void)
{
    bool dirty[CHIP_MAX];
    for (uint8_t i = 0; i < g_nchip; i++)
        dirty[i] = !chip_shown(&g_chip[i], g_chip_shown, g_nchip_shown);

    for (uint8_t j = 0; j < g_nchip_shown; j++) {
        if (chip_shown(&g_chip_shown[j], g_chip, g_nchip)) continue;
        for (uint8_t i = 0; i < g_nchip; i++)          /* under a vanished one */
            if (rect_overlap(&g_chip[i].r, &g_chip_shown[j].r))
                dirty[i] = true;
    }
    for (bool grew = true; grew; ) {                   /* under a dirty one */
        grew = false;
        for (uint8_t i = 0; i < g_nchip; i++) {
            if (!dirty[i]) continue;
            for (uint8_t k = 0; k < g_nchip; k++)
                if (!dirty[k] && rect_overlap(&g_chip[i].r, &g_chip[k].r)) {
                    dirty[k] = true;
                    grew = true;
                }
        }
    }

    for (uint8_t i = 0; i < g_nchip; i++)
        if (dirty[i])
            chip_paint(&g_chip[i]);
    memcpy(g_chip_shown, g_chip, sizeof(g_chip));
    g_nchip_shown = g_nchip;
}

/* ═══════════════════════════════════════════════════════════════════
 * Persistence overlay
 * ═══════════════════════════════════════════════════════════════════ */

static void draw_persistence_overlay(uint32_t frame)
{
    if (!persist_enabled) return;

    /* Feed demo waveform y-values into persistence buffer */
    static const int8_t sin_lut_p[64] = {
         0, 10, 19, 29, 38, 47, 56, 63, 71, 77, 83, 88, 92, 96, 98, 99,
        100, 99, 98, 96, 92, 88, 83, 77, 71, 63, 56, 47, 38, 29, 19, 10,
         0,-10,-19,-29,-38,-47,-56,-63,-71,-77,-83,-88,-92,-96,-98,-99,
       -100,-99,-98,-96,-92,-88,-83,-77,-71,-63,-56,-47,-38,-29,-19,-10,
    };

    uint16_t y_vals[PERSIST_WIDTH];
    uint16_t y_center = SCOPE_H / 2;
    for (uint16_t px = 0; px < PERSIST_WIDTH; px++) {
        uint8_t idx = (uint8_t)((px * 4 + frame) & 0x3F);
        int16_t yv = (int16_t)(y_center - (sin_lut_p[idx] * 40 / 100));
        if (yv < 0) yv = 0;
        if (yv >= PERSIST_HEIGHT) yv = PERSIST_HEIGHT - 1;
        y_vals[px] = (uint16_t)yv;
    }
    persist_add_trace(y_vals, 0);
    persist_decay();

    const uint8_t *buf = persist_get_buffer();
    if (!buf) return;

    for (uint16_t y = 0; y < PERSIST_HEIGHT; y++) {
        for (uint16_t x = 0; x < PERSIST_WIDTH; x++) {
            uint8_t intensity = buf[y * PERSIST_WIDTH + x];
            if (intensity > 0) {
                uint16_t color = persist_intensity_to_color_ch1(intensity);
                if (color != 0x0000)
                    lcd_set_pixel(x, y + SCOPE_TOP, color);
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Math channel overlay
 * ═══════════════════════════════════════════════════════════════════ */

static void draw_math_waveform(uint32_t frame)
{
    if (!math_enabled) return;

    const theme_t *th = theme_get();

    static const int8_t sin_lut_m[64] = {
         0, 10, 19, 29, 38, 47, 56, 63, 71, 77, 83, 88, 92, 96, 98, 99,
        100, 99, 98, 96, 92, 88, 83, 77, 71, 63, 56, 47, 38, 29, 19, 10,
         0,-10,-19,-29,-38,-47,-56,-63,-71,-77,-83,-88,-92,-96,-98,-99,
       -100,-99,-98,-96,-92,-88,-83,-77,-71,-63,-56,-47,-38,-29,-19,-10,
    };

    int16_t ch_a[LCD_WIDTH], ch_b[LCD_WIDTH], result[LCD_WIDTH];
    for (uint16_t x = 0; x < LCD_WIDTH; x++) {
        uint8_t idx = (uint8_t)((x * 4 + frame) & 0x3F);
        ch_a[x] = (int16_t)(sin_lut_m[idx] * 40);
        uint8_t phase = (uint8_t)((x * 4 + frame) & 0x3F);
        ch_b[x] = (int16_t)(phase < 32 ? -2500 : 2500);
    }

    math_config_t cfg;
    cfg.operation = (math_op_t)math_op;
    cfg.scale = 1.0f;
    math_channel_compute(ch_a, ch_b, result, LCD_WIDTH, &cfg);

    uint16_t y_center = SCOPE_MID_Y;
    for (uint16_t x = 0; x < LCD_WIDTH; x++) {
        int16_t y = (int16_t)(y_center - result[x] / 100);
        if (y >= SCOPE_TOP && y < SCOPE_BOT)
            lcd_set_pixel(x, (uint16_t)y, th->warning);
    }

    const char *name = math_channel_name((math_op_t)math_op);
    font_draw_string(130, SCOPE_TOP, "MATH:", th->warning, th->warning, &font_small);
    font_draw_string(170, SCOPE_TOP, name, th->warning, th->warning, &font_small);
}

/* ═══════════════════════════════════════════════════════════════════
 * Main scope screen compositor
 * ═══════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════
 * SPI3 Debug Overlay
 * Shows raw acquisition status for diagnosing scope data pipeline.
 * Off by default. Enable with -DSCOPE_DEBUG_OVERLAY at build time.
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef SCOPE_DEBUG_OVERLAY

/* SCOPE_DBG_Y is defined with the layout constants at the top of this file,
 * because the badge rows must be positioned relative to it. */
#define SCOPE_DBG_H   58

/*
 * GPIO pin scanner — REMOVED 2026-08-13.
 *
 * It accumulated per-port "this pin has toggled since boot" masks
 * (gpio_toggle_a..e) every frame, and NOTHING read them: the overlay line
 * that printed them was replaced during the config-entry experiments, and
 * the two derived counters (pb3/pb4_toggle_count) were assigned and never
 * used, which is why they showed up as unused statics in the build. An
 * instrument with no readout is not an instrument; this file has enough
 * history of numbers that could not mean what they appeared to mean.
 *
 * It is in git if it is ever wanted again: `git log -S gpio_toggle_a --
 * firmware/src/ui/scope_ui.c` (added in 5b7437a, last read before 134fa83).
 * Note the in-context MISO finder it once carried was already deleted in
 * 2026-04-06 because it drove SPI3 from the display task and corrupted the
 * acquisition task's transfers.
 */

/* Set by draw_scope_screen (whose full-area clear wipes 180-224) so the strip
 * background is refilled exactly when needed; the live incremental path never
 * dirties it, so per-frame text updates go straight over their own opaque
 * cells with no blank state — that blank-then-text sequence at frame rate was
 * half of the visible full-screen flashing. */
static uint8_t scope_dbg_strip_dirty = 1;

/* Pad to a fixed width so opaque redraw erases a previously-longer line
 * (counters shrink after a reset; P0 flips H/L width-stable but SS does not). */
static void dbg_pad(char *s, unsigned cap, unsigned width)
{
    unsigned l = 0;
    while (s[l] != '\0') l++;
    while (l < width && l + 1 < cap) s[l++] = ' ';
    s[l] = '\0';
}

static void draw_scope_debug(const theme_t *th)
{
    /* Dark background strip */
#if (defined(FPGA_PIN_SWEEP_BUILD) && FPGA_PIN_SWEEP_BUILD) || \
    (defined(FPGA_CFG_TRACE_BUILD) && FPGA_CFG_TRACE_BUILD) || \
    (defined(FPGA_IDCODE_PROBE) && FPGA_IDCODE_PROBE)
    /* Variant overlays draw variable-length content — keep the full refill. */
    lcd_fill_rect(0, SCOPE_DBG_Y, LCD_WIDTH, SCOPE_DBG_H, 0x0000);
#else
    if (scope_dbg_strip_dirty) {
        lcd_fill_rect(0, SCOPE_DBG_Y, LCD_WIDTH, SCOPE_DBG_H, 0x0000);
        scope_dbg_strip_dirty = 0;
    }
#endif

    char buf[64];

#if defined(FPGA_PIN_SWEEP_BUILD) && FPGA_PIN_SWEEP_BUILD
    /* ── RECONFIG_N pin sweep results ───────────────────────────────────────
     * Press SAVE in scope mode to run it. Read HIT first: it is the only line
     * that can say "we found something".
     *   HIT:-- H:0 AF:0  -> every candidate pulsed, nothing moved the STATUS
     *                       register. No MCU-owned pin stock drives is RECONFIG_N.
     *   HIT:PCn          -> that pin changed the status after CONFIG_ENABLE.
     *   AF non-zero      -> that many candidates failed the IDCODE anchor and
     *                       were DISCARDED, not counted as hits. Worth a look
     *                       anyway: a pin that CLOSES the config port would also
     *                       land here (Exp L behaviour). */
    {
        const char *st = (fpga.sweep_state == 0) ? "IDLE" :
                         (fpga.sweep_state == 1) ? "RUN"  : "DONE";
        snprintf(buf, sizeof(buf), "SWEEP:%s %u/%u", st,
                 (unsigned)fpga.sweep_tested, (unsigned)fpga.sweep_total);
        font_draw_string(2, SCOPE_DBG_Y + 2, buf, 0x07E0, 0x0000, &font_small);

        snprintf(buf, sizeof(buf), "BASE:%08lX",
                 (unsigned long)fpga.sweep_baseline);
        font_draw_string(2, SCOPE_DBG_Y + 15, buf, 0x07FF, 0x0000, &font_small);

        /* PH distinguishes two very different findings, which Exp O's single
         * snapshot could not tell apart:
         *   P = the PULSE ALONE moved the status — the pin triggered something
         *       on its own, which is what a real RECONFIG_N does (reload from NV).
         *   C = the status only moved after CONFIG_ENABLE — the pulse made 0x15
         *       land, i.e. it unlocked config entry. This is the jackpot. */
        const char *ph = (fpga.sweep_hit_phase == 1) ? "P" :
                         (fpga.sweep_hit_phase == 2) ? "C" : "-";
        snprintf(buf, sizeof(buf), "HIT:%s/%s %08lX",
                 fpga_sweep_pin_name(fpga.sweep_first_hit), ph,
                 (unsigned long)fpga.sweep_hit_status);
        font_draw_string(2, SCOPE_DBG_Y + 28, buf, 0xFFE0, 0x0000, &font_small);

        snprintf(buf, sizeof(buf), "H:%u AF:%u  press SAVE",
                 (unsigned)fpga.sweep_hits, (unsigned)fpga.sweep_anchor_fail);
        font_draw_string(2, SCOPE_DBG_Y + 41, buf, 0xF81F, 0x0000, &font_small);
    }
    return;
#endif

#if defined(FPGA_CFG_TRACE_BUILD) && FPGA_CFG_TRACE_BUILD
    /* ── Step-resolved anchored status trace ────────────────────────────────
     * Six checkpoints through the config sequence, each anchored on the IDCODE.
     * Checked in this order before drawing any conclusion:
     *
     *   A: line     the anchor at each checkpoint. '0' = IDCODE found aligned,
     *               'x' = anchor FAILED and that Tn is NOT a measurement. An 'x'
     *               appearing partway through is itself the finding: the config
     *               port closed at that step (Exp L behaviour).
     *   T0..T5      anchor-corrected STATUS. FFFFFFFF means "not measured".
     *
     * All six identical => the part ignores every config command while still
     * answering reads. Any step where it MOVES is where the sequence first has
     * an effect, and is where to aim next. */
    {
        snprintf(buf, sizeof(buf), "T0:%08lX T1:%08lX",
                 (unsigned long)fpga.cfg_trace[0], (unsigned long)fpga.cfg_trace[1]);
        font_draw_string(2, SCOPE_DBG_Y + 2, buf, 0x07E0, 0x0000, &font_small);

        snprintf(buf, sizeof(buf), "T2:%08lX T3:%08lX",
                 (unsigned long)fpga.cfg_trace[2], (unsigned long)fpga.cfg_trace[3]);
        font_draw_string(2, SCOPE_DBG_Y + 15, buf, 0x07FF, 0x0000, &font_small);

        snprintf(buf, sizeof(buf), "T4:%08lX T5:%08lX",
                 (unsigned long)fpga.cfg_trace[4], (unsigned long)fpga.cfg_trace[5]);
        font_draw_string(2, SCOPE_DBG_Y + 28, buf, 0xFFE0, 0x0000, &font_small);

        char a[FPGA_CFG_TRACE_N + 1];
        for (unsigned i = 0; i < FPGA_CFG_TRACE_N; i++) {
            int8_t o = fpga.cfg_trace_anchor[i];
            a[i] = (o < 0) ? 'x' : (char)('0' + (o % 10));
        }
        a[FPGA_CFG_TRACE_N] = '\0';
        /* MV = the count of checkpoints whose status differs from T0. 0 means the
         * register never moved anywhere in the sequence. */
        unsigned moved = 0;
        for (unsigned i = 1; i < FPGA_CFG_TRACE_N; i++)
            if (fpga.cfg_trace[i] != fpga.cfg_trace[0]) moved++;
        snprintf(buf, sizeof(buf), "A:%s MV:%u H2:%c", a, moved,
                 fpga.h2_upload_done ? 'Y' : 'N');
        font_draw_string(2, SCOPE_DBG_Y + 41, buf, 0xF81F, 0x0000, &font_small);
    }
    return;
#endif

#if defined(FPGA_IDCODE_PROBE) && FPGA_IDCODE_PROBE
    /* ── Experiment J overlay: the anchored opcode-discrimination probe ──────
     * Replaces the normal overlay entirely for this build. Four opcodes read at
     * /256 on a pristine bus before the prelude, 8 bytes each.
     *
     * READ IT LIKE THIS:
     *   ID / NP identical, and each showing a repeated 4-byte group
     *     => the FPGA free-runs a fixed pattern and ignores MOSI. Confirms the
     *        2026-06-13 conclusion on a valid measurement for the first time;
     *        SSPI cannot reach the config engine and JTAG is the route.
     *   ID differs from NP, ID@ >= 0
     *     => the FPGA DOES decode SSPI opcodes. The "not in config-receive mode"
     *        conclusion collapses and the config-entry search reopens.
     *   ID@ >= 0 but not 0
     *     => IDCODE present but phase-shifted by that many bits; the reply is
     *        real and our sampling alignment is off (see Exp I).
     */
    {
        const volatile uint8_t *p;

        p = fpga.probe_idcode;
        snprintf(buf, sizeof(buf), "ID:%02X%02X%02X%02X %02X%02X%02X%02X",
                 p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        font_draw_string(2, SCOPE_DBG_Y + 2, buf, 0x07E0, 0x0000, &font_small);

        p = fpga.probe_noop;
        snprintf(buf, sizeof(buf), "NP:%02X%02X%02X%02X %02X%02X%02X%02X",
                 p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        font_draw_string(2, SCOPE_DBG_Y + 15, buf, 0x07FF, 0x0000, &font_small);

        snprintf(buf, sizeof(buf), "ST:%02X%02X%02X%02X US:%02X%02X%02X%02X",
                 fpga.probe_status[0], fpga.probe_status[1],
                 fpga.probe_status[2], fpga.probe_status[3],
                 fpga.probe_user[0], fpga.probe_user[1],
                 fpga.probe_user[2], fpga.probe_user[3]);
        font_draw_string(2, SCOPE_DBG_Y + 28, buf, 0xFFE0, 0x0000, &font_small);

        /* ID@ = bit offset where 0x0120681B was found, -1 = absent.
         * SM  = 0x11 and 0x00 returned identical bytes (Y => no opcode decode).
         * RP  = the 0x11 reply repeats every 4 bytes (Y => free-running pattern).
         * PO@ = same IDCODE search on the 0x11 read taken AFTER CONFIG_ENABLE.
         * CL@ = same again AFTER the full upload and the 0x3A close. Per Exp L a
         *       configured part stops answering 0x11, so CL@ >= 0 means the config
         *       port never closed => we are definitively NOT configured. */
        snprintf(buf, sizeof(buf), "ID@%d SM:%c RP:%c PO@%d CL@%d",
                 (int)fpga.probe_id_bit,
                 fpga.probe_all_same ? 'Y' : 'N',
                 fpga.probe_repeats  ? 'Y' : 'N',
                 (int)fpga.probe_id_bit_post,
                 (int)fpga.probe_id_bit_close);
        font_draw_string(2, SCOPE_DBG_Y + 41, buf, 0xF81F, 0x0000, &font_small);
    }
    return;
#endif

    /* Line 1 (green): transport counters from the real FPGA path */
    /* Line 1 (green): post-upload scope-engine config readback.
     * SS = fpga.scope_status[] from the 0x03 status read — stock capture
     * returns 00 01 42 2E. CL = 0x3A close status — stock returns F8.
     * R  = first 4 raw CH1 bytes (pre-cal), to see if samples vary at all. */
    snprintf(buf, sizeof(buf), "SS:%02X%02X%02X%02X CL:%02X R:%02X%02X%02X%02X",
             fpga.scope_status[0], fpga.scope_status[1],
             fpga.scope_status[2], fpga.scope_status[3],
             fpga.h2_close_status,
             fpga.diag_ch1_raw[0], fpga.diag_ch1_raw[1],
             fpga.diag_ch1_raw[2], fpga.diag_ch1_raw[3]);
    dbg_pad(buf, sizeof(buf), 38);
    font_draw_string(2, SCOPE_DBG_Y + 2, buf,
                     0x07E0, 0x0000, &font_small);  /* green */

    /* Line 2 (cyan): acquisition state and critical control pins */
    snprintf(buf, sizeof(buf), "OK:%u TO:%u 1:%02X P0:%c P6:%c B:%c",
             (unsigned)fpga.spi3_ok_count,
             (unsigned)fpga.spi3_total_timeouts,
             fpga.spi3_first_byte,
             (GPIOC->idt & (1 << 0)) ? 'H' : 'L',
             (GPIOC->odt & (1 << 6)) ? 'H' : 'L',
             (GPIOB->odt & (1 << 11)) ? 'H' : 'L');
    dbg_pad(buf, sizeof(buf), 38);
    font_draw_string(2, SCOPE_DBG_Y + 15, buf,
                     0x07FF, 0x0000, &font_small);  /* cyan */

    /* Line 3 (yellow): Init handshake responses + raw PB4 state
     * G1[0-3] = sync+0x05+pad, G2[4-6] = 0x12+pad, G3[7-10] = 0x15+pad+0x3B
     * PB4: raw GPIO read of the MISO pin (1=HIGH/floating, 0=driven LOW) */
    /* L/H = min/max across the whole CH1 acquisition buffer. If L==H the
     * samples are a stuck constant (analog/trigger problem); if they spread,
     * real signal is arriving and the fault is downstream in rendering. */
    {
        const volatile uint8_t *cb = fpga_get_ch1_buf();
        uint8_t lo = 0xFF, hi = 0x00;
        for (uint32_t i = 0; i < FPGA_ADC_BUF_SIZE; i++) {
            uint8_t v = cb[i];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        /* CFG = Gowin STATUS_REGISTER (opcode 0x41) read right after the 0x3A
         * close — the authoritative "did the upload take?" verdict. Read at a
         * forced /256 since 2026-07-28; every earlier value was clocked at /2
         * and slipped one bit (the persistent 80 01 C8 10 is 0x00039020 read
         * one bit early — see fpga.c [6b]).
         *
         * D = DONE_FINAL (bit 13) — 1 means configuration completed. This, not
         *     SYSTEM_EDIT_MODE, is the success signal (Exp H showed stock reads
         *     the same 0x00039020 at the config-enable instant that we do, so
         *     ED cannot discriminate).
         * E = the error nibble, bits 3..0 = TIMEOUT | ID_VERIFY_FAILED |
         *     BAD_COMMAND | CRC_ERROR. Non-zero means bytes DID reach the config
         *     engine and were rejected → a content/transport bug, localizable.
         *     Zero with D=0 means the engine never received anything at all. */
        uint32_t sr = ((uint32_t)fpga.cfg_status_reg[0] << 24) |
                      ((uint32_t)fpga.cfg_status_reg[1] << 16) |
                      ((uint32_t)fpga.cfg_status_reg[2] << 8)  |
                      ((uint32_t)fpga.cfg_status_reg[3]);
        /* A = the post-0x3A IDCODE anchor: the bit offset at which 0x0120681B was
         *     found in the read taken immediately before CFG, at the same clock.
         *     A>=0  -> CFG was read on a validated path AND the config port is
         *             still open, i.e. we are definitively NOT configured.
         *     A=-1  -> either the port closed (Exp L: what a successfully
         *             configured part does) or the bus is dead. CFG is then
         *             UNTRUSTWORTHY on its own — corroborate with a live trace. */
        snprintf(buf, sizeof(buf), "CFG:%08lX D%u E%X A%d L%u H%u",
                 (unsigned long)sr,
                 (unsigned)((sr >> 13) & 1u),
                 (unsigned)(sr & 0x0Fu),
                 (int)fpga.probe_id_bit_close,
                 (unsigned)lo, (unsigned)hi);
    }
    dbg_pad(buf, sizeof(buf), 38);
    font_draw_string(2, SCOPE_DBG_Y + 28, buf,
                     0xFFE0, 0x0000, &font_small);  /* yellow */

    /* Line 4 (magenta): SPI3 CTRL1 + the EDIT_MODE probe + H2 upload flag.
     * S1 = SPI3 CTRL1 as latched during the config frame. Bits[5:3] are BR, so
     *      0347 = /2 (stock, Exp F fidelity build) and 037F = /256. This is the
     *      on-device proof of which clock the config frame actually ran at.
     * ED = STATUS_REGISTER (0x41) read at /256 immediately after 0x15.
     *      SYSTEM_EDIT_MODE is bit 7 of the ASSEMBLED 32-bit word, i.e. bit 7 of
     *      ED[3] — NOT bit 7 of ED[0], as this comment and scripts/swd_fpga_status.sh
     *      both said until 2026-07-28. (Same verdict either way for 0x00039020:
     *      ED[3]=0x20, bit 7 clear. But the test was reading bit 31.)
     *      Exp H demoted this from "the wall" to a non-discriminator: stock reads
     *      the same 0x00039020 here and still configures. Compare CFG's D flag.
     * Dropped S2/ST here — CTRL2 and STS were static (0003 / 0002) across every
     * run so far, and ED is the number this experiment turns on.
     * H2T means bytes streamed, not recovered FPGA acceptance. */
    snprintf(buf, sizeof(buf), "S1:%04X ED:%02X%02X%02X%02X H2T:%c",
             (uint16_t)fpga.diag_spi_ctrl1,
             fpga.edit_mode_status[0], fpga.edit_mode_status[1],
             fpga.edit_mode_status[2], fpga.edit_mode_status[3],
             fpga.h2_upload_done ? 'Y' : 'N');
    dbg_pad(buf, sizeof(buf), 38);
    font_draw_string(2, SCOPE_DBG_Y + 41, buf,
                     0xF81F, 0x0000, &font_small);  /* magenta */
}

#endif /* SCOPE_DEBUG_OVERLAY */

/* ═══════════════════════════════════════════════════════════════════
 * X-Y (Lissajous) view — CH1 drives X, CH2 drives Y
 *
 * Plots the two ADC buffers against each other instead of against time:
 * op04 (CH1) -> horizontal, op05 (CH2) -> vertical. The buffers come from
 * one synchronised acquisition and were bench-confirmed time-aligned
 * (~20 deg phase spread, EXP-21 2026-08-21), so (ch1[i], ch2[i]) is a
 * single instant and a Lissajous figure renders coherently.
 *
 * CH2's readout is a dead buffer until its vertical-offset reference
 * (TMR13 CH1 PWM-DAC on PA6) is armed — the same thing `trig2 raw` does
 * from the shell — so this view arms it once on first entry. Square plot
 * area (side = SCOPE_H) so frequency ratios are not distorted.
 *
 * Reads the volatile buffers directly (no tear-check copy): a frame the
 * acq task is mid-updating just displaces a few points, cosmetically
 * negligible here. Consecutive samples are connected so the locus reads as
 * a continuous trace, the way an analog scope draws X-Y.
 * ═══════════════════════════════════════════════════════════════════ */

/* Bresenham segment written into the persistence buffer (buffer coords). */
static void xy_line_persist(int x0, int y0, int x1, int y1)
{
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = (dx > dy ? dx : -dy) / 2, e2;
    for (;;) {
        persist_add_point((uint16_t)x0, (uint16_t)y0);
        if (x0 == x1 && y0 == y1) break;
        e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 <  dy) { err += dx; y0 += sy; }
    }
}

void draw_xy_screen(void)
{
    const theme_t *th = theme_get();
    const uint16_t side = SCOPE_H;                 /* square side, 206 px */
    const uint16_t x0   = (LCD_WIDTH - side) / 2;  /* centred: 57 */
    const uint16_t y0   = SCOPE_TOP;               /* 18 */
    const uint16_t cx   = x0 + side / 2;
    const uint16_t cy   = y0 + side / 2;

    /* One-time bring-up: offset DACs (CH1 DAC1 + CH2 TMR13/PA6) and the shared
     * persistence buffer, borrowed here for the phosphor trail (X-Y and the
     * time-domain persistence overlay never run at once). */
    static bool xy_init = false;
    static int  xy_dac1 = 2048, xy_dac2 = 2048;
    if (!xy_init) {
        scope_trigger_dac_init();
        scope_trigger_ch2_init();
        scope_trigger_dac_raw((uint16_t)xy_dac1);
        scope_trigger_ch2_raw((uint16_t)xy_dac2);
        if (!persist_is_initialized()) persist_init();
        persist_set_mode(PERSIST_LOW);             /* short phosphor tail */
        persist_clear();
        xy_init = true;
    }

    /* Margins + title: background repaint of the strips either side of the
     * square (also wipes a prior view on entry). The square is streamed from
     * the persistence buffer below and never blanks, so there is no flash. */
    lcd_fill_rect(0, SCOPE_TOP, x0, SCOPE_H, th->background);
    lcd_fill_rect((uint16_t)(x0 + side), SCOPE_TOP,
                  (uint16_t)(LCD_WIDTH - (x0 + side)), SCOPE_H, th->background);
    font_draw_string(4, SCOPE_TOP + 2, "X-Y", th->text_secondary,
                     th->background, &font_small);

    const volatile uint8_t *xb = fpga_get_ch1_buf();
    const volatile uint8_t *yb = fpga_get_ch2_buf();
    bool have = fpga_data_ready() && xb != NULL && yb != NULL
                && persist_is_initialized();

    /* Snapshot buffers live on the FreeRTOS heap, not in .bss: these 2 KB
     * put the guest-flavour links over the RAM ceiling (make guest stopped
     * linking at 352 B over on main alone, before fw_loader's buffers even
     * joined it — #29). Allocated once, on first X-Y frame with data, and
     * kept; on this heap (32 KB, ~14 KB measured free) the failure path is
     * decorative, but it fails to "no snapshot", never to a null deref. */
    static uint8_t *xy_xs, *xy_ys;
    if (have && (xy_xs == NULL || xy_ys == NULL)) {
        if (xy_xs == NULL) xy_xs = pvPortMalloc(FPGA_ADC_BUF_SIZE);
        if (xy_ys == NULL) xy_ys = pvPortMalloc(FPGA_ADC_BUF_SIZE);
        if (xy_xs == NULL || xy_ys == NULL) have = false;
    }

    persist_decay();                     /* fade the whole buffer once per frame */

    if (have) {
        /* Coherent snapshot (odd generation = acq commit in progress). */
        uint8_t *const xs = xy_xs;
        uint8_t *const ys = xy_ys;
        for (int t = 0; t < 4; t++) {
            uint32_t g0 = fpga_acq_frame_generation();
            memcpy(xs, (const void *)xb, FPGA_ADC_BUF_SIZE);
            memcpy(ys, (const void *)yb, FPGA_ADC_BUF_SIZE);
            if ((g0 & 1u) == 0u && fpga_acq_frame_generation() == g0)
                break;
        }

        uint8_t xmin = 255, xmax = 0, ymin = 255, ymax = 0;
        for (uint16_t i = 0; i < FPGA_ADC_BUF_SIZE; i++) {
            if (xs[i] < xmin) xmin = xs[i];
            if (xs[i] > xmax) xmax = xs[i];
            if (ys[i] < ymin) ymin = ys[i];
            if (ys[i] > ymax) ymax = ys[i];
        }
        const int xmid_raw = (xmin + xmax) / 2;
        const int ymid_raw = (ymin + ymax) / 2;
        int rng_raw = (xmax - xmin) > (ymax - ymin) ? (xmax - xmin) : (ymax - ymin);
        if (rng_raw < 4) rng_raw = 4;

        /* Smoothed auto-scale: rounded EMA (alpha 1/8) of centre and span. */
        static int xmid = 128, ymid = 128, rng = 64;
        static bool seeded = false;
        if (!seeded) { xmid = xmid_raw; ymid = ymid_raw; rng = rng_raw; seeded = true; }
        else {
            xmid += ((xmid_raw - xmid) + (xmid_raw >= xmid ? 4 : -4)) / 8;
            ymid += ((ymid_raw - ymid) + (ymid_raw >= ymid ? 4 : -4)) / 8;
            rng  += ((rng_raw  - rng ) + (rng_raw  >= rng  ? 4 : -4)) / 8;
        }
        if (rng < 4) rng = 4;

        /* Auto-centre servo on the offset DACs (baseline -> ADC mid-scale). */
        int e1 = 128 - xmid_raw, e2 = 128 - ymid_raw;
        if (e1 > 4 || e1 < -4) {
            xy_dac1 += e1 * 5;
            if (xy_dac1 < 0) xy_dac1 = 0; else if (xy_dac1 > 4095) xy_dac1 = 4095;
            scope_trigger_dac_raw((uint16_t)xy_dac1);
        }
        if (e2 > 4 || e2 < -4) {
            xy_dac2 += e2 * 5;
            if (xy_dac2 < 0) xy_dac2 = 0; else if (xy_dac2 > 4095) xy_dac2 = 4095;
            scope_trigger_ch2_raw((uint16_t)xy_dac2);
        }

        /* Plot into the persistence buffer, buffer coords (x 0..319,
         * y 0..PERSIST_HEIGHT-1 = screen y - SCOPE_TOP). Y inverted. */
        const int half  = side / 2 - 6;
        const int chord = side / 3;
        const int xl = (int)x0, xh = (int)x0 + (int)side - 1;
        const int yl = 0,       yh = (int)side - 1;
        const int cyb = (int)cy - (int)SCOPE_TOP;
        int pbx = 0, pby = 0;
        for (uint16_t i = 0; i < FPGA_ADC_BUF_SIZE; i++) {
            int bx = (int)cx + ((int)xs[i] - xmid) * half * 2 / rng;
            int by = cyb - ((int)ys[i] - ymid) * half * 2 / rng;
            if (bx < xl) bx = xl; else if (bx > xh) bx = xh;
            if (by < yl) by = yl; else if (by > yh) by = yh;
            if (i > 0) {
                int adx = bx > pbx ? bx - pbx : pbx - bx;
                int ady = by > pby ? by - pby : pby - by;
                if (adx < chord && ady < chord)
                    xy_line_persist(pbx, pby, bx, by);
                else
                    persist_add_point((uint16_t)bx, (uint16_t)by);
            } else {
                persist_add_point((uint16_t)bx, (uint16_t)by);
            }
            pbx = bx; pby = by;
        }
    }

    /* Stream the square from the persistence buffer, compositing the centre
     * axes. Empty pixels -> background, so faded points erase cleanly and the
     * square is repainted with no blank phase (no flash). */
    const uint8_t *pb = persist_get_buffer();
    uint16_t xy_row[SCOPE_H];            /* stack, not BSS — one row of the square */
    const int cxr = (int)cx - (int)x0;      /* centre col within the square */
    const int cyr = (int)cy - (int)y0;      /* centre row within the square */
    for (int ry = 0; ry < (int)side; ry++) {
        for (int rx = 0; rx < (int)side; rx++) {
            uint8_t inten = pb ? pb[ry * PERSIST_WIDTH + (int)x0 + rx] : 0;
            uint16_t c;
            if (inten)
                c = persist_intensity_to_color_ch1(inten);
            else if (rx == cxr || ry == cyr)
                c = th->grid;
            else
                c = th->background;
            xy_row[rx] = c;
        }
        lcd_set_window(x0, (uint16_t)(y0 + ry), side, 1);
        lcd_write_pixels(xy_row, side);
    }

    if (!have)
        font_draw_string_center(LCD_WIDTH / 2, SCOPE_MID_Y, "X-Y: no signal",
                                th->warning, th->background, &font_small);
}

/*
 * The trigger point (horizontal position) on the glass, for both renderers.
 *
 * A 7 x 4 triangle pointing down, just under the top row of labels, at the
 * column where the trigger landed; grey at the REQUESTED column when nothing
 * was locked this frame (a flat record, free run), so LEFT/RIGHT always show
 * where they put it. While the arrows move it (MOVE Trigger / Position) a
 * dotted line also runs down the band, crossing the level line there.
 *
 * Until 2026-11-26 the triangle sat on the band's first rows, under the
 * top-row labels: the SPI3 counter, the STOP pill (which covers the default
 * mid-screen column), the mode tag and the trigger status hid it over most
 * of the width, and it was not drawn at all without a lock.
 */
typedef struct {
    int16_t  x, y0;
    uint16_t color;
    bool     line;
} trig_point_t;

static trig_point_t trig_point_get(const scope_state_t *ss, const theme_t *th)
{
    trig_point_t t;
    const bool locked = (g_trig_x_actual >= 0);
    t.x     = locked ? g_trig_x_actual : ss->trig_x;
    t.y0    = (int16_t)(SCOPE_TOP + font_small.height + 2 + 1);
    t.color = locked ? th->trigger : th->text_secondary;
    const move_mode_t m = scope_move_mode_now();
    t.line  = (m == MOVE_TRIGGER || m == MOVE_POSITION);
    return t;
}

static inline bool trig_point_tri(const trig_point_t *t, int x, int y)
{
    const int r = y - t->y0;
    int d = x - t->x;
    if (d < 0) d = -d;
    return r >= 0 && r < 4 && d <= 3 - r;
}

static inline bool trig_point_dot(const trig_point_t *t, int x, int y)
{
    return t->line && x == t->x && y > t->y0 + 3 && ((y - SCOPE_TOP) % 3) == 0;
}

void draw_scope_screen(uint32_t frame)
{
    const theme_t *th = theme_get();
    const scope_state_t *ss = scope_state_get();

    /* Clear waveform area */
    lcd_fill_rect(0, SCOPE_TOP, LCD_WIDTH, SCOPE_H, th->background);

    /* Layer 1: Persistence overlay (under everything) */
    draw_persistence_overlay(frame);

    /* Layer 2: Grid */
    draw_scope_grid();

    /* Layer 3: the channel reference markers are band chips (layer 9),
     * drawn by both renderers; the old full-path-only ground markers sat at
     * mid-screen whatever the channel's band, and the first live frame
     * erased them (removed 2026-11-26). */

    /* Layer 4: Trigger level indicator (this also finds where the trigger
     * point lands, g_trig_x_actual), and the trigger-point line under the
     * traces, as the compositor draws it. */
    draw_trigger_indicator(ss, th, trigger_marker_y_full(ss));
    const trig_point_t tp = trig_point_get(ss, th);
    if (tp.line)
        for (int y = tp.y0 + 4; y < TRACE_BOT; y++)
            if (trig_point_dot(&tp, tp.x, y))
                lcd_set_pixel((uint16_t)tp.x, (uint16_t)y, tp.color);

    /* Layer 5: Waveform */
    draw_demo_waveform(frame);

    /* Mask strip at the bottom of the live band (the compositor paints the
     * same rows), only when draw_demo_waveform() armed the overlay. */
    draw_mask_strip(th, BADGE_ROW2_Y);

    /* Trigger-point marker (trig_point_get: where it landed, or grey where
     * it was asked to land when nothing locked). */
    {
        const trig_point_t tp2 = trig_point_get(ss, th);
        for (int r = 0; r < 4; r++)
            for (int dx = -(3 - r); dx <= 3 - r; dx++) {
                const int x = tp2.x + dx;
                if (x >= 0 && x < LCD_WIDTH)
                    lcd_set_pixel((uint16_t)x, (uint16_t)(tp2.y0 + r), tp2.color);
            }
    }

    /* Layer 6: Math channel overlay */
    draw_math_waveform(frame);

    /* Layer 7: Cursor lines */
    draw_cursors(ss, th);

    /* Layer 8: Measurement badges (below the band) */
    draw_measurement_badges(ss, th);

    /* Layer 9: Band chips: top row, cursor labels, cursor readout. The same
     * chips the live compositor paints, built after the traces noted their
     * scales (the dV readout reads them). */
    scope_chips_build(ss, th);
    /* The reference markers' tips, beside their chips (chan_ref_px). */
    for (int y = SCOPE_TOP; y < TRACE_BOT; y++)
        for (int x = REF_CHIP_W; x < REF_CHIP_W + 3; x++) {
            uint16_t rc;
            if (chan_ref_px(x, y, &rc))
                lcd_set_pixel((uint16_t)x, (uint16_t)y, rc);
        }
    scope_chips_paint();

    /* Layer 10: Quick-change popup (on top of everything) */
    draw_popup(th);

#ifdef SCOPE_DEBUG_OVERLAY
    /* Layer 11: SPI3 debug overlay (bottom of screen) */
    scope_dbg_strip_dirty = 1;   /* the :1050-style full clear wiped 180-224 */
    draw_scope_debug(th);
#endif
}

/* ═══════════════════════════════════════════════════════════════════
 * Live incremental frame (2026-08-12 rendering pass)
 *
 * Flicker-free trace update for the real-data path: repaints the waveform
 * band column-by-column, compositing each pixel's FINAL color (trace over
 * trigger line over crosshair over grid over background) into one streamed
 * window write per column — the screen never holds a blank state, unlike
 * draw_scope_screen's clear-then-redraw, which is the visible flash. Grid
 * pixels are re-plotted arithmetically (there is no framebuffer to restore
 * from; pattern mirrors draw_scope_grid exactly). Cost: one lcd_set_window
 * (~13 us) per column segment + streamed pixel writes. This said "6-8 ms per
 * frame, 20 fps capable"; measured on my unit it is ~84 ms of wall time
 * (DWT, `display` shell command, EXP-85), preemption by the acquisition task
 * included, which caps the scope at ~8 frames/s. Keep columns to one segment
 * where possible: each extra segment is another lcd_set_window.
 *
 * Cursor lines and the trigger arrow are composited per pixel (cursor_px,
 * trig_arrow_px: the same tests the full path draws with). Text on the band
 * (the popup and the chips: top-row labels, cursor labels, cursor readout)
 * is cut out as holes and painted opaque after the stream, so every band
 * pixel is written exactly once per frame (band_holes.h, EXP-85). Still full
 * path only: the badges (below the band), the ground markers and the math
 * overlay.
 * ═══════════════════════════════════════════════════════════════════ */
void draw_scope_live_frame(void)
{
    const theme_t *th = theme_get();
    const scope_state_t *ss = scope_state_get();
    const volatile uint8_t *b1 = fpga_get_ch1_buf();
    const volatile uint8_t *b2 = fpga_get_ch2_buf();

    if (!b1 || !b2) return;

    /* Software trigger — THIS is the per-frame live path, so this is where the
     * free-run "dancing" actually happens. Shift both channels' render window
     * to the trigger source's crossing, using the same helper and offset as the
     * full path (draw_demo_waveform) so the two renderers stay in phase. Both
     * channels shift by the same amount to stay time-aligned. */
    {
        const volatile uint8_t *trig_src =
            (ss->trigger.source == TRIG_SRC_CH2) ? b2 : b1;
        uint16_t toff = scope_soft_trigger_offset(ss, trig_src);
        b1 += toff;
        b2 += toff;
    }
    mask_overlay_begin();          /* per-column mask colours, see draw_mask_strip */

    /* The live band stops where fixed furniture begins: the debug strip
     * (debug builds) or the measurement badge rows. Those regions repaint
     * themselves; streaming over them would z-fight at frame rate. */
    /* Always the badge rows: they are the topmost fixed furniture in BOTH
     * configurations. In overlay builds the badges now sit ABOVE the debug
     * strip (see BADGE_ROW_Y), so stopping at SCOPE_DBG_Y — as this did until
     * 2026-08-14 — streamed the waveform straight over them at frame rate.
     * That was the second occlusion bug in a row here: first the debug strip
     * painted over the badges, then moving the badges up put them inside the
     * live band. Anchor to the badges and both cases are covered. */
    const uint16_t band_bot = BADGE_ROW2_Y;
    const uint16_t band_h = band_bot - SCOPE_TOP;
    (void)band_h;


    /* The full path's maps exactly (chan_map_prep), band split, true scale
     * and the channel's vertical position included, so a change of renderer
     * never moves the trace. Both paths end the trace at TRACE_BOT ==
     * band_bot, so neither leaves pixels in the readout strip (EXP-77).
     *
     * Until 2026-11-26 the position was applied by neither path (one of them
     * applying it on its own is what let the two disagree once); it now
     * lives in the shared map, so they cannot. */
    const bool en1 = ss->ch1.enabled;
    const bool en2 = ss->ch2.enabled;

    autofit_t a1 = { 0 };
    autofit_t a2 = { 0 };
    vband_clear();
    if (en1) {
        chan_map_prep(&a1, b1, en1 && en2, 0u, chan_fixed(ss, 1u), 0);
        vband_note(0u, 1u, ss->ch1.vdiv_idx, a1.y_top, a1.y_bot,
                   autofit_counts_per_pixel(&a1));
    }
    if (en2) {
        chan_map_prep(&a2, b2, en1 && en2, 1u, chan_fixed(ss, 2u), 1);
        vband_note((en1 && en2) ? 1u : 0u, 2u, ss->ch2.vdiv_idx, a2.y_top, a2.y_bot,
                   autofit_counts_per_pixel(&a2));
    }

    /* Trigger dotted line, placed exactly as the full path places it: on the
     * trigger source's map, at the record code where the hardware fires. */
    int16_t trig_y = trigger_marker_y(ss, (ss->trigger.source == TRIG_SRC_CH2) ? &a2 : &a1);
    if (trig_y < SCOPE_TOP + 2) trig_y = SCOPE_TOP + 2;
    if (trig_y > TRACE_BOT - 3) trig_y = TRACE_BOT - 3;
    const uint16_t trig_step = (scope_move_mode_now() == MOVE_TRIGGER) ? 1u : 3u;   /* dot mask */

    int16_t p1 = 0, p2 = 0;  /* previous column's y — vertical continuity */

    const cursor_state_t *cur = &ss->cursor;
    const bool trig_rising = (ss->trigger.edge == TRIG_RISING);
    /* g_trig_x_actual was set by the soft trigger above, this frame. */
    const trig_point_t tp = trig_point_get(ss, th);
    /* Cursor and arrow tests are hoisted out of the pixel loop: a pixel only
     * asks cursor_px() when it sits on a cursor column or row, and the arrow
     * only in its 8 x 5 box. 0xFFFF = no such row. */
    const bool cur_v = (cur->mode == CURSOR_VERTICAL || cur->mode == CURSOR_BOTH);
    const bool cur_h = (cur->mode == CURSOR_HORIZONTAL || cur->mode == CURSOR_BOTH);
    const uint16_t cur_hy1 = cur_h ? cur->h1_y : 0xFFFFu;
    const uint16_t cur_hy2 = cur_h ? cur->h2_y : 0xFFFFu;

    /* Holes: the popup (painted once by scope_popup_overlay_tick) and this
     * frame's chips (painted below, after the stream). */
    scope_chips_build(ss, th);
    band_rect_t holes[BAND_MAX_HOLES];
    uint8_t nh = 0;
    if (popup_frames > 0)
        holes[nh++] = popup_rect();
    for (uint8_t i = 0; i < g_nchip && nh < BAND_MAX_HOLES; i++)
        holes[nh++] = g_chip[i].r;

    for (uint16_t x = 0; x < LCD_WIDTH; x++) {
        /* One transform, shared with the full path — see autofit_prep. */
        int16_t y1 = a1.on ? autofit_y(&a1, b1[x]) : (int16_t)SCOPE_MID_Y;
        int16_t y2 = a2.on ? autofit_y(&a2, b2[x]) : (int16_t)SCOPE_MID_Y;

        /* 2-px dot plus a span to the previous sample, so steep edges draw
         * as connected verticals instead of the full path's dotted gaps. */
        int16_t lo1 = y1, hi1 = y1 + 1, lo2 = y2, hi2 = y2 + 1;
        if (x > 0) {
            if (p1 < lo1) lo1 = p1;
            if (p1 > hi1) hi1 = p1;
            if (p2 < lo2) lo2 = p2;
            if (p2 > hi2) hi2 = p2;
        }
        p1 = y1;
        p2 = y2;

        /* Mask overlay for this column: the strip colour and whether either
         * trace is painted as outside the mask. */
        bool     mdot = false;
        uint16_t mstrip = mask_strip_color((int)x, th, &mdot);
        uint16_t tc1 = mask_px_fail(0, (int)x) ? th->warning : th->ch1;
        uint16_t tc2 = mask_px_fail(1, (int)x) ? th->warning : th->ch2;
        /* Mask bounds (dotted, every other column), through each band's own
         * autofit -- the one transform, as everywhere else. -1 = none. */
        int16_t bl1 = -1, bh1 = -1, bl2 = -1, bh2 = -1;
        if (g_mask_bounds_on && MASK_BOUND_DOT(x)) {
            uint8_t lo, hi;
            if (a1.on && scope_mask_bound_at(0, (int)x, g_trig_x_actual, &lo, &hi)) {
                bl1 = autofit_y(&a1, lo); bh1 = autofit_y(&a1, hi);
            }
            if (a2.on && scope_mask_bound_at(1, (int)x, g_trig_x_actual, &lo, &hi)) {
                bl2 = autofit_y(&a2, lo); bh2 = autofit_y(&a2, hi);
            }
        }

        /* The popup is a HOLE in the live band, not a layer: its glyphs are
         * the one thing this compositor cannot recompute per pixel, so the
         * column is emitted as up to two segments that step around the box.
         * When the countdown ends the hole closes and the very next live
         * frame paints the band back over it — no full repaint at either
         * end, which is the whole point of the change. */
        const bool cur_col = cur_v && (x == cur->v1_x || x == cur->v2_x);
        const bool arrow_col = (x >= LCD_WIDTH - 8);
        const int  tp_d = ((int)x > tp.x) ? (int)x - tp.x : tp.x - (int)x;
        const bool tp_col = (tp_d <= 3);
        const bool tp_line = tp.line && tp_d == 0;
        const bool ref_col = (x >= REF_CHIP_W && x < REF_CHIP_W + 3);

        uint16_t seg_top[BAND_MAX_SEGS], seg_bot[BAND_MAX_SEGS];
        const uint8_t nseg = band_col_segments(x, holes, nh, SCOPE_TOP, band_bot,
                                               seg_top, seg_bot);

        for (uint8_t sg = 0; sg < nseg; sg++) {
        if (seg_bot[sg] <= seg_top[sg]) continue;
        lcd_set_window(x, seg_top[sg], 1, (uint16_t)(seg_bot[sg] - seg_top[sg]));
        for (uint16_t y = seg_top[sg]; y < seg_bot[sg]; y++) {
            uint16_t c;
            /* Z-order matches the full path: CH2 painted after CH1 there,
             * so CH2 wins here; trace over trigger line over grid. */
            if ((cur_col || y == cur_hy1 || y == cur_hy2) &&
                cursor_px(cur, th, x, y, &c))
                ;                                      /* cursor lines on top */
            else if (tp_col && trig_point_tri(&tp, (int)x, (int)y))
                c = tp.color;                          /* trigger-point marker */
            else if (ref_col && chan_ref_px((int)x, (int)y, &c))
                ;                                      /* channel reference tips */
            else if (arrow_col && (uint16_t)(y - trig_y + 2) <= 4u &&
                     trig_arrow_px((int)x, (int)y, trig_y, trig_rising))
                c = th->trigger;
            else if (mstrip && y >= band_bot - MASK_STRIP_H &&
                     (!mdot || ((x + y) & 1u) == 0u))
                c = mstrip;
            else if (a2.on && (int16_t)y >= lo2 && (int16_t)y <= hi2)
                c = tc2;
            else if (a1.on && (int16_t)y >= lo1 && (int16_t)y <= hi1)
                c = tc1;
            else if ((int16_t)y == bl1 || (int16_t)y == bh1 ||
                     (int16_t)y == bl2 || (int16_t)y == bh2)
                c = th->text_secondary;
            else if ((int16_t)y == trig_y && (x & trig_step) == 0 && x < LCD_WIDTH - 6)
                c = th->trigger;
            else if (tp_line && trig_point_dot(&tp, (int)x, (int)y))
                c = tp.color;                          /* trigger-point line */
            else if (y == SCOPE_MID_Y || x == LCD_WIDTH / 2)
                c = th->grid_center;
            else if (((x & 31u) == 0 && (y & 1u) == 0) ||
                     (((y - SCOPE_TOP) % 26u) == 0 && (x & 1u) == 0))
                c = th->grid;
            else
                c = th->background;
            lcd_write_data(c);
        }
        }
    }

    /* The chips go into their holes last, only those that changed. Each
     * writes every pixel of its box once, so a moved or rewritten chip never
     * leaves a blank. */
    scope_chips_paint_changed();

    /* ⚠ Measurement badges are NOT refreshed here — REVERTED 2026-08-14.
     * DO NOT REINSTATE WITHOUT READING THIS.
     *
     * Calling draw_measurement_badges() from this function KILLED ACQUISITION
     * on the bench: OK: reached exactly 20 and stopped, TO: climbed forever,
     * and it survived two full FPGA power cycles and a pinhole reset.
     * Reproduced on BOTH guest-persist and guest-coldtrace, so it was not the
     * settings-persistence work. Before the edit, OK: ran past 191,000.
     *
     * Why: the measurement passes read 1024 samples from BOTH channel buffers,
     * three times each, from the DISPLAY task — while the acquisition task is
     * writing those same buffers over SPI3.
     *
     * The badges do need refreshing (they otherwise latch "--" from scope
     * entry, where fpga.c has just cleared data_ready, and B2's redraw gating
     * means no further full repaint is triggered). But the fix belongs on the
     * other side: give the redraw epoch a measurement-generation term so a
     * full repaint happens when new samples land, instead of doing 6 KB of
     * analysis inline on the capture-rate path.
     *
     * Cost of leaving it out: stale badges — a display bug. The version that
     * was here was a data-loss bug. Prefer the display bug. */

#ifdef SCOPE_DEBUG_OVERLAY
    /* Keep the counters moving; opaque fixed-width lines, no strip refill. */
    draw_scope_debug(th);
#endif
}

/* ═══════════════════════════════════════════════════════════════════
 * FFT views (unchanged — reference scope_state for consistency)
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef FEATURE_FFT

/* ── Display reference auto-ranging ─────────────────────────────────
 *
 * fft_process() reports magnitude in dB relative to ONE ADC COUNT
 * (20*log10(|X| * 2/N), |X| in counts) — not dBFS. A full-scale signal
 * therefore peaks near 20*log10(32767) = 90 dB, and the configured default
 * ref_level_db = 0.0 puts the top of the display ~90 dB BELOW the signal:
 * every populated bin clamps to normalized = 0, i.e. the top colour of the
 * ramp. Measured on the host with the same synthetic 1 kHz square the UI
 * feeds itself (scratch probe, 2026-08-13): peak = 84.4 dB, and 318 of the
 * 320 waterfall columns land in the RED band. That is the "hot red field
 * with thin nulls" seen on the bench the same day.
 *
 * Fix: track the observed peak instead of hard-coding a magic 90. The
 * reference follows frame_max + headroom, moving up immediately (so a
 * growing signal never clips against the top) and easing down slowly
 * through a dead band (so a jittering peak does not make the colours
 * breathe). cfg->ref_level_db is preserved as a USER TRIM applied on top,
 * which keeps fft_adjust_ref_level() meaningful.
 *
 * Known limit, stated rather than hidden: this ranges off the peak only. A
 * spectrum with no dynamic range (broadband noise and nothing else) has its
 * mean within ~10-15 dB of its peak and will still render as a flat, fairly
 * hot field. That is a true statement about such a signal; distinguishing it
 * would need a floor estimator as well, which is not warranted until real
 * captured data (rather than the synthetic square below) drives this view.
 */
#define FFT_REF_HEADROOM_DB   6.0f   /* keep the peak just below the top    */
#define FFT_REF_DEAD_BAND_DB  3.0f   /* ignore downward wobble below this   */
#define FFT_REF_DECAY         0.10f  /* per-frame fraction on the way down  */

static float fft_ref_tracked = 0.0f;
static bool  fft_ref_primed  = false;

/* ── Where the spectrum views get their samples ──────────────────────
 *
 * Until 2026-08-13 every FFT view — spectrum, split and waterfall — called
 * test_signal_generate() unconditionally and analysed a synthetic 1 kHz
 * square. Cold-boot capture started working that same day and none of them
 * lit up, because none of them was ever connected to the ADC. Worse, the
 * views did not say so: a user could not tell the spectrum of their probe
 * from the spectrum of a constant compiled into the firmware.
 *
 * Since then: the live CH1 record if there is one, and the view is LABELLED
 * with what it got. Same gate as the trace (fpga_data_ready()), same
 * tear-check as the measurement badges (frame generation counter).
 *
 * 2026-09-15 (FFT-live S1):
 *  - The record's invalid head is skipped (SCOPE_RECORD_HEAD_SKIP, the
 *    trace's SEAM_GUARD): 896 samples of static body go into the transform,
 *    windowed over the record rather than over the transform length
 *    (fft.c FFT_WINDOW_AT).
 *  - The frequency axis is REAL on measured timebase codes: the rate comes
 *    from scope_timebase_sample_rate() for the code in force, and is
 *    refused (0 -> bins, "--") when the code is unmeasured OR when the
 *    display's code and the FPGA's reg-0x01 code disagree — the same rule
 *    `fpga scope freq` applies. A confident wrong number is the failure
 *    mode this project keeps finding; "--" is not.
 *  - In the live-capture builds (FPGA_WARM_HANDOFF_TEST) the synthetic
 *    source is compiled OUT: with no capture yet the views say so and draw
 *    nothing, so the demo signal is unreachable there by construction
 *    (spec S1). Plain `guest` and `emu` keep the demo, labelled DEMO.
 */
typedef enum {
    FFT_SRC_NONE = 0,   /* live build, no capture yet: nothing to analyse */
    FFT_SRC_DEMO,       /* synthetic 1 kHz square at FFT_DEMO_SAMPLE_RATE_HZ */
    FFT_SRC_LIVE        /* CH1 acquisition record, head skipped */
} fft_src_t;

#define FFT_DEMO_SAMPLE_RATE_HZ  44100.0f

#if defined(FPGA_WARM_HANDOFF_TEST) && FPGA_WARM_HANDOFF_TEST
#define FFT_DEMO_REACHABLE 0
#else
#define FFT_DEMO_REACHABLE 1
#endif

/* Sample rate the live spectrum may claim, 0 when it may not. */
static float fft_live_rate_in_force(void)
{
    const scope_state_t *ss = scope_state_get();
    if (fpga_acq_rate_idx_get() != ss->timebase_idx)
        return 0.0f;                         /* display and hardware disagree */
    return scope_timebase_sample_rate(ss->timebase_idx);
}

static fft_src_t fft_prepare_input(int16_t *sbuf, uint16_t *n_out,
                                   float *fs_out)
{
    const volatile uint8_t *b = fpga_get_ch1_buf();
    *n_out = 0;
    *fs_out = 0.0f;

    if (fpga_data_ready() && b != NULL) {
        uint16_t n = 0;
        for (int attempt = 0; attempt < 3; attempt++) {
            uint32_t g0 = fpga_acq_frame_generation();
            n = fft_live_prepare((const uint8_t *)b, FPGA_ADC_BUF_SIZE,
                                 sbuf, FFT_SIZE);
            if ((g0 & 1u) == 0u && fpga_acq_frame_generation() == g0)
                break;
        }
        *fs_out = fft_live_rate_in_force();
        fft_set_sample_rate(*fs_out);
        *n_out = n;
        return FFT_SRC_LIVE;
    }

#if FFT_DEMO_REACHABLE
    fft_set_sample_rate(FFT_DEMO_SAMPLE_RATE_HZ);
    test_signal_generate(TEST_SIG_SQUARE, sbuf,
                         FFT_SIZE, FFT_DEMO_SAMPLE_RATE_HZ,
                         1000.0f, 0.0f, 0.8f);
    *fs_out = FFT_DEMO_SAMPLE_RATE_HZ;
    *n_out = FFT_SIZE;
    return FFT_SRC_DEMO;
#else
    return FFT_SRC_NONE;
#endif
}

/* Frequency-axis end labels for the visible bin span: hertz on a measured
 * code, "--" otherwise. Drawn along the bottom edge of the region. */
static void draw_fft_axis_labels(uint16_t y_bot, float fs,
                                 uint16_t zoom_start, uint16_t zoom_end)
{
    char lbl[12];
    fft_live_axis_label(fs, FFT_SIZE, zoom_start, lbl, sizeof(lbl));
    /* Opaque on black: the bars fill the bottom of the region, and grey
     * transparent text on amber bars was invisible on the bench (FFT-live
     * S2, 2026-09-22). fg == bg means transparent in font_draw_char(). */
    font_draw_string(2, y_bot - 10, lbl, COLOR_WHITE, COLOR_BLACK, &font_small);
    fft_live_axis_label(fs, FFT_SIZE, zoom_end, lbl, sizeof(lbl));
    font_draw_string_right(LCD_WIDTH - 2, y_bot - 10, lbl,
                           COLOR_WHITE, COLOR_BLACK, &font_small);
}

static float fft_display_ref(const fft_config_t *cfg, const float *data,
                             uint16_t start_bin, uint16_t end_bin)
{
    if (end_bin >= FFT_BINS) end_bin = FFT_BINS - 1;
    if (start_bin > end_bin) start_bin = end_bin;

    float peak = data[start_bin];
    uint16_t b;
    for (b = (uint16_t)(start_bin + 1); b <= end_bin; b++)
        if (data[b] > peak) peak = data[b];

    float target = peak + FFT_REF_HEADROOM_DB;

    if (!fft_ref_primed) {
        fft_ref_tracked = target;
        fft_ref_primed = true;
    } else if (target > fft_ref_tracked) {
        fft_ref_tracked = target;                       /* attack: immediate */
    } else if (fft_ref_tracked - target > FFT_REF_DEAD_BAND_DB) {
        fft_ref_tracked += (target - fft_ref_tracked) * FFT_REF_DECAY;
    }

    return fft_ref_tracked + cfg->ref_level_db;
}

static void draw_fft_region(uint16_t y_top, uint16_t height)
{
    const fft_config_t *cfg = fft_get_config();
    uint16_t y_bot = y_top + height;

    int16_t *sbuf = fft_get_sample_buf();
    if (!sbuf) return;  /* FFT not initialized */
    uint16_t nsamp = 0;
    float fs = 0.0f;
    fft_src_t src = fft_prepare_input(sbuf, &nsamp, &fs);
    if (src == FFT_SRC_NONE || nsamp == 0) {
        font_draw_string(4, y_top + 2, "FFT: waiting for capture",
                         COLOR_GRAY, COLOR_GRAY, &font_small);
        return;
    }
    bool live = (src == FFT_SRC_LIVE);
    fft_process(sbuf, nsamp, &fft_result);

    const float *draw_data = (fft_result.avg_db != NULL)
                             ? fft_result.avg_db : fft_result.level_db;

    uint16_t zoom_start = cfg->zoom_start_bin;
    uint16_t zoom_end = cfg->zoom_end_bin;
    uint16_t zoom_span = zoom_end - zoom_start;
    uint16_t x;

    /* Same auto-ranged reference the waterfall uses: with the configured
     * ref_level_db = 0 every bar pinned to full height, because the dB scale
     * is relative to one count, not to full scale. See fft_display_ref(). */
    float ref_db = fft_display_ref(cfg, draw_data, zoom_start, zoom_end);
    float range_db = cfg->db_range;

    for (x = 0; x < LCD_WIDTH; x++) {
        uint16_t bin = zoom_start + (uint16_t)((uint32_t)x * zoom_span / LCD_WIDTH);
        if (bin >= FFT_BINS) bin = FFT_BINS - 1;

        float db = draw_data[bin];
        float normalized = (ref_db - db) / range_db;
        if (normalized < 0.0f) normalized = 0.0f;
        if (normalized > 1.0f) normalized = 1.0f;

        uint16_t bar_top = y_top + (uint16_t)(normalized * (float)height);
        if (bar_top < y_bot) {
            uint16_t color;
            if (normalized < 0.25f) color = COLOR_CH1;
            else if (normalized < 0.5f) color = COLOR_GREEN;
            else if (normalized < 0.75f) color = COLOR_CYAN;
            else color = COLOR_GRID;
            lcd_fill_rect(x, bar_top, 1, y_bot - bar_top, color);
        }

        if (fft_result.max_hold_db != NULL) {
            float mh_norm = (ref_db - fft_result.max_hold_db[bin]) / range_db;
            if (mh_norm >= 0.0f && mh_norm <= 1.0f) {
                uint16_t mh_y = y_top + (uint16_t)(mh_norm * (float)height);
                if (mh_y >= y_top && mh_y < y_bot && (x & 1) == 0)
                    lcd_set_pixel(x, mh_y, COLOR_RED);
            }
        }
    }

    /* Horizontal grid lines (every 10dB) */
    float db;
    for (db = ref_db; db > ref_db - range_db; db -= 10.0f) {
        float normalized = (ref_db - db) / range_db;
        uint16_t y = y_top + (uint16_t)(normalized * (float)height);
        if (y > y_top && y < y_bot)
            for (x = 0; x < LCD_WIDTH; x += 4)
                lcd_set_pixel(x, y, COLOR_GRID);
    }

    if (height > 80) {
        for (db = ref_db; db > ref_db - range_db; db -= 20.0f) {
            float normalized = (ref_db - db) / range_db;
            uint16_t y = y_top + (uint16_t)(normalized * (float)height);
            if (y > y_top + 8 && y < y_bot - 8) {
                int db_int = (int)db;
                char label[8];
                int pos = 0;
                if (db_int < 0) { label[pos++] = '-'; db_int = -db_int; }
                if (db_int >= 100) label[pos++] = (char)('0' + db_int / 100);
                if (db_int >= 10)  label[pos++] = (char)('0' + (db_int / 10) % 10);
                label[pos++] = (char)('0' + db_int % 10);
                label[pos] = '\0';
                font_draw_string(2, y - 5, label, COLOR_GRAY, COLOR_BLACK, &font_small);  /* opaque: see draw_fft_axis_labels */
            }
        }
    }

    uint8_t p;
    for (p = 0; p < fft_result.num_peaks && p < 3; p++) {
        uint16_t peak_bin = fft_result.peaks[p].bin;
        if (peak_bin < zoom_start || peak_bin > zoom_end) continue;

        uint16_t peak_x = (uint16_t)((uint32_t)(peak_bin - zoom_start)
                          * LCD_WIDTH / zoom_span);
        float norm = (ref_db - fft_result.peaks[p].level_db) / range_db;
        if (norm < 0.0f) norm = 0.0f;
        uint16_t peak_y = y_top + (uint16_t)(norm * (float)height);

        if (peak_y >= y_top + 4 && peak_x > 2 && peak_x < LCD_WIDTH - 2) {
            lcd_set_pixel(peak_x, peak_y - 3, COLOR_RED);
            lcd_set_pixel(peak_x - 1, peak_y - 2, COLOR_RED);
            lcd_set_pixel(peak_x, peak_y - 2, COLOR_RED);
            lcd_set_pixel(peak_x + 1, peak_y - 2, COLOR_RED);
        }

        /* The per-peak harmonic tags ("Fund", "H2", ...) are RATIOS between
         * bins, so they stay true whatever the sample rate turns out to be —
         * unlike an absolute Hz figure, which is why one is drawn here and
         * the other is not. */
        if (fft_result.peaks[p].label[0] != '\0' && peak_x > 8 && peak_x < LCD_WIDTH - 30) {
            /* Keep the tag below the source/peak header, which occupies
             * y_top + 2 .. y_top + 2 + font height. The tallest peak reaches
             * the headroom line just under the header, so its tag sat on top
             * of "LIVE CH1 pk ..." on the bench (2026-09-22). */
            int16_t tag_y = (int16_t)peak_y - 12;
            int16_t tag_min = (int16_t)y_top + 2 + (int16_t)font_small.height + 1;
            if (tag_y < tag_min) tag_y = tag_min;
            font_draw_string(peak_x - 8, (uint16_t)tag_y,
                             fft_result.peaks[p].label, COLOR_ORANGE, COLOR_ORANGE, &font_small);
        }
    }

    /* Source + peak header. The peak is quoted in Hz only when the sample
     * rate is known — the demo signal's by construction, the live record's
     * when the timebase code in force is a measured one (fft_live_axis_known).
     * Otherwise it is quoted as a bin index, which needs no rate to be true. */
    {
        char hdr[40];
        char freq_str[16];
        uint16_t color = live ? COLOR_WHITE : COLOR_ORANGE;
        const char *tag = live ? "LIVE CH1" : "DEMO sq";
        if (fft_result.num_peaks == 0) {
            snprintf(hdr, sizeof(hdr), "%s", live ? "LIVE CH1" : "DEMO SIGNAL");
        } else if (fft_live_axis_known(fs)) {
            fft_live_format_hz(fft_result.peaks[0].freq_hz, freq_str,
                               sizeof(freq_str));
            snprintf(hdr, sizeof(hdr), "%s  pk %s", tag, freq_str);
        } else {
            snprintf(hdr, sizeof(hdr), "%s  pk bin %u  --", tag,
                     (unsigned)fft_result.peaks[0].bin);
        }
        font_draw_string(4, y_top + 2, hdr, color, color, &font_small);
    }

    if (height > 80)
        draw_fft_axis_labels(y_bot, fs, zoom_start, zoom_end);

    const char *win_names[] = { "Rect", "Hann", "Hamm", "BHar", "Flat" };
    const char *win_name = (cfg->window < FFT_WINDOW_COUNT)
                           ? win_names[cfg->window] : "?";
    font_draw_string_right(LCD_WIDTH - 4, y_top + 2, win_name,
                           COLOR_GRAY, COLOR_GRAY, &font_small);
}

void draw_fft_screen(void)
{
    lcd_fill_rect(0, SCOPE_TOP, LCD_WIDTH, SCOPE_H, COLOR_BLACK);
    draw_fft_region(SCOPE_TOP + 2, SCOPE_H - 4);
}

void draw_split_screen(uint32_t frame)
{
    lcd_fill_rect(0, SCOPE_TOP, LCD_WIDTH, SCOPE_H, COLOR_BLACK);

    uint16_t scope_top = SCOPE_TOP;
    uint16_t scope_bot = SCOPE_TOP + SCOPE_H / 2 - 1;
    uint16_t scope_mid = (scope_top + scope_bot) / 2;
    uint16_t x;

    for (x = 0; x < LCD_WIDTH; x += 32)
        for (uint16_t y = scope_top; y < scope_bot; y += 2)
            lcd_set_pixel(x, y, COLOR_GRID);
    for (x = 0; x < LCD_WIDTH; x++)
        lcd_set_pixel(x, scope_mid, COLOR_GRID_CENTER);

    /* Top half: the real CH1 record when there is one. Half-height, so the
     * same y-transform as the full trace with SCOPE_H replaced by the band
     * height — one screen column is still one sample. */
    const volatile uint8_t *b1 = fpga_get_ch1_buf();
    bool trace_live = fpga_data_ready() && b1 != NULL;

    if (trace_live) {
        uint16_t band_h = (uint16_t)(scope_bot - scope_top);
        /* Start past the invalid head (scope_record.h), like the trace. */
        for (x = 0; x < LCD_WIDTH && x + SCOPE_RECORD_HEAD_SKIP < FPGA_ADC_BUF_SIZE; x++) {
            int16_t wy = (int16_t)scope_mid -
                         (int16_t)(((int16_t)b1[x + SCOPE_RECORD_HEAD_SKIP] - 128) * band_h / 256);
            if (wy >= (int16_t)scope_top && wy < (int16_t)scope_bot)
                lcd_set_pixel(x, (uint16_t)wy, COLOR_CH1);
        }
    } else {
        static const int8_t sin_lut[64] = {
             0, 10, 19, 29, 38, 47, 56, 63, 71, 77, 83, 88, 92, 96, 98, 99,
            100, 99, 98, 96, 92, 88, 83, 77, 71, 63, 56, 47, 38, 29, 19, 10,
             0,-10,-19,-29,-38,-47,-56,-63,-71,-77,-83,-88,-92,-96,-98,-99,
           -100,-99,-98,-96,-92,-88,-83,-77,-71,-63,-56,-47,-38,-29,-19,-10,
        };
        for (x = 0; x < LCD_WIDTH; x++) {
            uint8_t idx = (uint8_t)((x * 4 + frame) & 0x3F);
            int16_t wy = scope_mid - (sin_lut[idx] * 25 / 100);
            if (wy >= (int16_t)scope_top && wy < (int16_t)scope_bot)
                lcd_set_pixel(x, (uint16_t)wy, COLOR_CH1);
        }
        /* An animated sine that no probe produced. Say so. */
        font_draw_string(4, scope_top + 2, "DEMO TRACE",
                         COLOR_ORANGE, COLOR_ORANGE, &font_small);
    }

    uint16_t divider_y = SCOPE_TOP + SCOPE_H / 2;
    for (x = 0; x < LCD_WIDTH; x++)
        lcd_set_pixel(x, divider_y, COLOR_GRID_CENTER);

    draw_fft_region(divider_y + 1, SCOPE_H / 2 - 2);
}

/* Waterfall
 *
 * The 20 KB history buffer lives in the shared pool as a sub-tenant of the
 * FFT region (see SHMEM_FFT_WATERFALL_OFFSET) rather than in .bss. It cannot
 * be a pool owner of its own: this function calls fft_process(), so it only
 * ever runs while SHMEM_OWNER_FFT holds the pool, and claiming it separately
 * would evict the FFT it depends on.
 *
 * Only waterfall_row_idx, the generation counter and one assembled screen
 * line stay static — 645 bytes instead of 20,480.
 */
#define WATERFALL_ROWS  SHMEM_FFT_WATERFALL_ROWS
#define WATERFALL_COLS  SHMEM_FFT_WATERFALL_COLS
static uint8_t waterfall_row_idx = 0;
static uint32_t waterfall_pool_generation = 0;

/* The row blit below sets one window across the full plot width per history
 * row and streams into it, so a history row must be exactly one screen line
 * wide. */
_Static_assert(WATERFALL_COLS == LCD_WIDTH,
               "waterfall row blit assumes one history column per screen pixel");

/*
 * One assembled screen line, 640 bytes. Colours are converted once per
 * history row and pushed row_height times, instead of being recomputed for
 * every sub-row. It is deliberately NOT the 20 KB history buffer's problem:
 * that lives in the shared pool (see shared_mem.h) precisely because it is
 * large, whereas one line is small enough that the pool's ownership
 * lifetime rules would cost more than they save.
 */
static uint16_t waterfall_line[WATERFALL_COLS];

/*
 * Intensity -> colour, with a BLACK floor.
 *
 * intensity 0 = at/above the display reference, 255 = at/below the floor
 * (reference - db_range). The old ramp ended at pure blue, so the noise
 * floor painted the plot solid blue and only the very top 20% of the scale
 * was red; combined with the reference bug (see fft_display_ref) that made
 * the whole plot one flat colour. This one runs black -> blue -> cyan ->
 * yellow -> red in four equal 64-step segments, which is the conventional
 * spectrogram ordering and puts "nothing here" at black.
 */
static uint16_t intensity_to_color(uint8_t intensity)
{
    uint16_t level = (uint16_t)(255u - intensity);   /* 0 = floor, 255 = hot */
    uint8_t  t = (uint8_t)((level & 63u) * 4u);      /* position in segment  */

    if (level < 64)  return RGB565(0, 0, t);                        /* black->blue  */
    if (level < 128) return RGB565(0, t, 255);                      /* blue ->cyan  */
    if (level < 192) return RGB565(t, 255, (uint8_t)(255 - t));     /* cyan ->yellow*/
    return RGB565(255, (uint8_t)(255 - t), 0);                      /* yellow->red  */
}

void draw_waterfall_screen(void)
{
    const fft_config_t *cfg = fft_get_config();

    int16_t *sbuf = fft_get_sample_buf();
    if (!sbuf) return;  /* FFT not initialized */
    uint16_t nsamp = 0;
    float fs = 0.0f;
    fft_src_t src = fft_prepare_input(sbuf, &nsamp, &fs);
    if (src == FFT_SRC_NONE || nsamp == 0) {
        lcd_fill_rect(0, SCOPE_TOP, LCD_WIDTH, SCOPE_H, COLOR_BLACK);
        font_draw_string(4, SCOPE_TOP + 2, "WFALL: waiting for capture",
                         COLOR_GRAY, COLOR_GRAY, &font_small);
        return;
    }
    bool live = (src == FFT_SRC_LIVE);
    fft_process(sbuf, nsamp, &fft_result);

    /* Resolve the history buffer out of the FFT's pool tenancy. Returns NULL
     * if FFT no longer owns the pool, in which case there is nothing valid to
     * draw into — bail rather than scribble over another owner's data. This is
     * a stricter check than the fft_get_sample_buf() guard above, which only
     * proves the FFT was initialised at some point. */
    uint8_t *wf_pool = shared_mem_get(SHMEM_OWNER_FFT);
    if (!wf_pool) return;
    uint8_t (*waterfall_buf)[WATERFALL_COLS] =
        (uint8_t (*)[WATERFALL_COLS])(wf_pool + SHMEM_FFT_WATERFALL_OFFSET);

    /* shared_mem_acquire() zeroes the pool whenever the owner changes, so a
     * round trip through screenshot/persistence wipes the history. Detect that
     * and restart from row 0 instead of scrolling through a zeroed tail.
     *
     * Refill with 0xFF rather than leaving the pool's zeros: intensity 0 maps
     * to the top of the ramp (red = at the display reference), so zeroed rows
     * would render as maximum signal. 0xFF is the other end of the ramp
     * (black = reference - db_range = the noise floor), which is what "no data
     * yet" should look like. Bench 2026-08-13: with the zero fill, entering
     * WFALL painted the whole plot red and the history grew downward
     * through it. */
    uint32_t pool_generation = shared_mem_transition_count();
    if (pool_generation != waterfall_pool_generation) {
        waterfall_pool_generation = pool_generation;
        waterfall_row_idx = 0;
        memset(waterfall_buf, 0xFF, SHMEM_FFT_WATERFALL_SIZE);
    }

    const float *draw_data = (fft_result.avg_db != NULL)
                             ? fft_result.avg_db : fft_result.level_db;

    uint16_t zoom_start = cfg->zoom_start_bin;
    uint16_t zoom_end = cfg->zoom_end_bin;
    uint16_t zoom_span = zoom_end - zoom_start;
    if (zoom_span == 0) zoom_span = 1;

    float ref_db = fft_display_ref(cfg, draw_data, zoom_start, zoom_end);

    uint16_t x;
    for (x = 0; x < WATERFALL_COLS; x++) {
        uint16_t bin = zoom_start + (uint16_t)((uint32_t)x * zoom_span / WATERFALL_COLS);
        if (bin >= FFT_BINS) bin = FFT_BINS - 1;
        float db = draw_data[bin];
        float normalized = (ref_db - db) / cfg->db_range;
        if (normalized < 0.0f) normalized = 0.0f;
        if (normalized > 1.0f) normalized = 1.0f;
        waterfall_buf[waterfall_row_idx][x] = (uint8_t)(normalized * 255.0f);
    }

    uint8_t newest_row = waterfall_row_idx;
    waterfall_row_idx = (uint8_t)((waterfall_row_idx + 1) % WATERFALL_ROWS);

    /* ── Blit ───────────────────────────────────────────────────────
     *
     * This used to clear the whole plot and then draw every pixel column as
     * its own 1 x row_height lcd_fill_rect: 20,481 fill_rects and therefore
     * 20,481 lcd_set_window() calls per frame, each one 3 commands + 8 byte
     * writes with a bus delay after every single one — ~225,000 delayed bus
     * transactions to paint 61,440 pixels. Bench 2026-08-13: the plot
     * visibly rastered left-to-right, top-to-bottom over seconds.
     *
     * Now: one window per history row, and each row is converted once into
     * waterfall_line and streamed row_height times. 65 set_windows and 192
     * lcd_write_pixels() calls per frame, with the same pixels at the same
     * coordinates.
     *
     * One window for the WHOLE block would cost 2 set_windows instead of 65,
     * but the window is global driver state and vInputTask runs at priority 4
     * against the display task's 1: its power-off countdown overlay
     * (input_handler.c) calls lcd_fill_rect() and so re-points the window
     * from under a stream in progress. Per-row windows bound that to one
     * corrupted row for one frame — the same blast radius the per-column
     * version had — for ~700 extra bus transactions out of 66,000.
     *
     * The clear is gone too: the block covers every pixel it draws over, so
     * the only part of the plot that still needs clearing is the residual
     * strip left when SCOPE_H is not a multiple of WATERFALL_ROWS (14 rows
     * at the current 206/64). That halves the pixel writes as well.
     *
     * Why not scroll-and-append, which would be O(1 row)? The history
     * scrolls by exactly one row per frame, so 63 of 64 rows are unchanged
     * in CONTENT — but they all MOVE, and with no framebuffer on this side
     * of the bus there is nothing to shift; the ST7789's own vertical
     * scroll works on panel rows, which MADCTL = 0xA0 (MV = 1) maps to
     * screen COLUMNS, i.e. it would scroll the waterfall sideways. An
     * O(1)-per-frame waterfall needs either a RAM framebuffer (the shared
     * pool is already spoken for by the FFT here) or a different history
     * layout that lets the newest row wrap in place instead of the image
     * scrolling — which changes what is drawn, so it is a design decision,
     * not a speedup. */
    uint16_t row_height = SCOPE_H / WATERFALL_ROWS;
    if (row_height < 1) row_height = 1;

    uint16_t rows_drawn = WATERFALL_ROWS;
    if ((uint32_t)rows_drawn * row_height > SCOPE_H)
        rows_drawn = (uint16_t)(SCOPE_H / row_height);
    uint16_t block_h = (uint16_t)(rows_drawn * row_height);

    if (block_h < SCOPE_H)
        lcd_fill_rect(0, (uint16_t)(SCOPE_TOP + block_h), LCD_WIDTH,
                      (uint16_t)(SCOPE_H - block_h), COLOR_BLACK);

    uint16_t r;
    for (r = 0; r < rows_drawn; r++) {
        const uint8_t *src =
            waterfall_buf[(newest_row + WATERFALL_ROWS - r) % WATERFALL_ROWS];
        for (x = 0; x < WATERFALL_COLS; x++)
            waterfall_line[x] = intensity_to_color(src[x]);

        lcd_set_window(0, (uint16_t)(SCOPE_TOP + r * row_height),
                       WATERFALL_COLS, row_height);

        uint16_t rep;
        for (rep = 0; rep < row_height; rep++)
            lcd_write_pixels(waterfall_line, WATERFALL_COLS);
    }

    /* Say which signal built this history. A waterfall of a compiled-in test
     * tone looks exactly like a waterfall of a probe. */
    if (live)
        font_draw_string(4, SCOPE_TOP + 2, "WFALL LIVE CH1",
                         COLOR_WHITE, COLOR_WHITE, &font_small);
    else
        font_draw_string(4, SCOPE_TOP + 2, "WFALL DEMO SIGNAL",
                         COLOR_ORANGE, COLOR_ORANGE, &font_small);
    draw_fft_axis_labels(SCOPE_TOP + SCOPE_H, fs, zoom_start, zoom_end);
}

#endif /* FEATURE_FFT */
