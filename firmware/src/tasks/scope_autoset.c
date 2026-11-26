/*
 * OpenScope 2C53T: the autoset run. See scope_autoset.h and ui/autoset.h.
 */
#include "scope_autoset.h"

#include "autoset.h"
#include "fpga.h"
#include "input_handler.h"
#include "move_mode.h"
#include "scope_cal.h"
#include "scope_record.h"
#include "scope_state.h"
#include "scope_timebase.h"
#include "scope_trigger.h"
#include "ui.h"

#include "task.h"
#include <stdio.h>
#include <string.h>

typedef enum { AS_OFF = 0, AS_VERT, AS_FREQ, AS_VERT2 } as_phase_t;

#define AS_N           ((uint16_t)(FPGA_ADC_BUF_SIZE - SCOPE_RECORD_HEAD_SKIP))
#define AS_VERT_ITERS  6u

/* Fast to slow: a signal is counted at the first rate that shows it, so a
 * fast one is never counted aliased at a slow rate. */
static const uint8_t freq_codes[] = { 0x08, 0x0B, 0x0E, 0x11, 0x14 };

static struct {
    as_phase_t phase;
    uint32_t   gen_wait;        /* measure once the generation reaches this */
    TickType_t deadline;
    uint8_t    iter, fidx, nfreq;
    uint8_t    tb_start, src;   /* src: 1/2, 0 = none */
    bool       settled[2], has[2];
    float      vpp[2], vmean[2], mid[2];   /* volts at the BNC; mid in counts */
    float      hz;
} as;

static scope_state_t *ss_(void) { return scope_state_get(); }
static channel_state_t *chs(uint8_t ch) { return ch == 2u ? &ss_()->ch2 : &ss_()->ch1; }
static bool ch_on(uint8_t ch) { return chs(ch)->enabled; }
static uint16_t off_code(uint8_t ch)
{
    return ch == 2u ? scope_trigger_ch2_last() : scope_trigger_dac_last();
}

/* Peak to peak at most this many divisions: about 85 % of the rows a
 * channel has (a half, 86 rows = 3.3 div, with both on; 175 rows = 6.7 div
 * alone). The first rule, 2.5 / 5.0, put the generator's 0-3 V square
 * (3.16 Vpp with its overshoot, 2.51 div on range 6) on range 7 at 1.25 div
 * (bench, 2026-11-26). */
static float target_div(void)
{
    return (ch_on(1) && ch_on(2)) ? 2.8f : 5.5f;
}

/* Two commits after now: the first may hold a capture from before the change. */
static void wait_fresh(void)
{
    const float fs = scope_timebase_sample_rate(ss_()->timebase_idx);
    uint32_t ms = 1500u;
    if (fs > 0.0f)
        ms += (uint32_t)(2.0f * (float)FPGA_ADC_BUF_SIZE * 1000.0f / fs);
    const uint32_t g = fpga_acq_frame_generation();
    as.gen_wait = ((g + 1u) & ~1u) + 4u;
    as.deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
}

/* Levels of a channel's record, read in place under the generation check
 * (no copy: RAM is tight). False if a commit kept landing mid-read. */
static bool measure(uint8_t ch, autoset_stats_t *st, float *cps)
{
    const volatile uint8_t *b = (ch == 2u) ? fpga_get_ch2_buf() : fpga_get_ch1_buf();
    if (b == NULL || !fpga_data_ready())
        return false;
    for (int a = 0; a < 3; a++) {
        const uint32_t g0 = fpga_acq_frame_generation();
        if (g0 & 1u)
            continue;
        const uint8_t *p = (const uint8_t *)b + SCOPE_RECORD_HEAD_SKIP;
        autoset_stats(p, AS_N, st);
        if (cps)
            *cps = autoset_cycles_per_sample(p, AS_N, st->lo, st->hi);
        if (fpga_acq_frame_generation() == g0)
            return true;
    }
    return false;
}

/* Counts -> volts at the BNC (the calibration table's own units, which the
 * range picker uses; the probe factor is a display matter). */
static bool bnc_volts(uint8_t ch, float counts, float *v)
{
    const uint8_t r = chs(ch)->vdiv_idx;
    const float g = scope_cal_mv_per_count(ch, r) / 1000.0f;
    float z;
    /* Offset code 0: never armed (emulator, early boot), so no zero, as
     * scope_chan_zero_counts() says too. */
    if (g <= 0.0f || off_code(ch) == 0u || !scope_cal_zero_counts(ch, r, off_code(ch), &z))
        return false;
    *v = (counts - z) * g;
    return true;
}

static void set_range(uint8_t ch, uint8_t r)
{
    channel_state_t *c = chs(ch);
    if (c->vdiv_idx == r)
        return;
    c->vdiv_idx = r;
    (void)fpga_apply_vdiv(ch, r);
}

static void set_timebase(uint8_t code)
{
    ss_()->timebase_idx = code;
    (void)fpga_apply_timebase(code);
}

static void finish(QueueHandle_t dq, const char *msg)
{
    as.phase = AS_OFF;
    scope_show_popup(msg);
    input_settings_tick();          /* the result is saved like a key press */
    uint8_t cmd = DCMD_SCOPE_LIVE;
    xQueueSend(dq, &cmd, 0);
}

/* One vertical pass over the channels not yet settled. True when done. */
static bool vert_step(void)
{
    bool done = true;
    for (uint8_t ch = 1u; ch <= 2u; ch++) {
        const uint8_t i = (uint8_t)(ch - 1u);
        if (!ch_on(ch) || as.settled[i])
            continue;
        autoset_stats_t st;
        if (!measure(ch, &st, NULL))
            return false;               /* a commit in the way: next tick */
        const uint8_t r = chs(ch)->vdiv_idx;
        float vmin = 0.0f, vmax = 0.0f, vmean = 0.0f;
        const bool vok = bnc_volts(ch, (float)st.lo, &vmin) &&
                         bnc_volts(ch, (float)st.hi, &vmax) &&
                         bnc_volts(ch, st.mean, &vmean);
        as.vpp[i] = vmax - vmin;
        as.vmean[i] = vmean;
        as.mid[i] = ((float)st.lo + (float)st.hi) / 2.0f;
        as.has[i] = vok && !st.clipped ? autoset_has_signal(as.vpp[i], vmean) : true;
        uint8_t next;
        if (st.clipped)
            next = (r >= 8u) ? 9u : (uint8_t)(r + 2u);
        else if (vok)
            next = autoset_pick_range(ch, off_code(ch), vmin, vmax, target_div());
        else
            next = 7u;                  /* an uncalibrated range: start mid-ladder */
        if (next == r) {
            as.settled[i] = true;
        } else {
            set_range(ch, next);
            done = false;
        }
    }
    as.iter++;
    return done || as.iter >= AS_VERT_ITERS;
}

/* The channel to trigger on and count: the source if it has a signal, else
 * the other one if it does, else the source anyway (to look for one). */
static uint8_t pick_source(void)
{
    const uint8_t cur = (ss_()->trigger.source == TRIG_SRC_CH2) ? 2u : 1u;
    const uint8_t oth = (cur == 1u) ? 2u : 1u;
    if (ch_on(cur) && as.has[cur - 1u]) return cur;
    if (ch_on(oth) && as.has[oth - 1u]) return oth;
    return ch_on(cur) ? cur : oth;
}

static void start_vert(as_phase_t p)
{
    as.phase = p;
    as.iter = 0u;
    as.settled[0] = as.settled[1] = false;
}

static void freq_step(void)
{
    autoset_stats_t st;
    float cps = 0.0f;
    if (!measure(as.src, &st, &cps))
        return;                         /* next tick */
    const float fs = scope_timebase_sample_rate(freq_codes[as.fidx]);
    const float cycles = cps * (float)AS_N;
    if (cps > 0.0f && cycles >= 3.0f && fs > 0.0f) {
        as.hz = cps * fs;
    } else if (++as.fidx < as.nfreq) {
        set_timebase(freq_codes[as.fidx]);
        wait_fresh();
        return;
    } else {
        as.hz = 0.0f;                   /* DC or slower than the ladder */
    }
    const uint8_t code = autoset_pick_timebase(as.hz);
    set_timebase(code == 0xFFu ? as.tb_start : code);
    start_vert(AS_VERT2);
    wait_fresh();
}

static void finish_ok(QueueHandle_t dq)
{
    scope_state_t *ss = ss_();
    bool any = false;
    for (uint8_t ch = 1u; ch <= 2u; ch++) {
        const uint8_t i = (uint8_t)(ch - 1u);
        if (!ch_on(ch))
            continue;
        channel_state_t *c = chs(ch);
        float z;
        if (as.has[i] && off_code(ch) != 0u &&
            scope_cal_zero_counts(ch, c->vdiv_idx, off_code(ch), &z)) {
            c->position = autoset_position_rows(as.mid[i], z);
            any = true;
        } else {
            set_range(ch, 5u);          /* nothing there: the boot range, 0 V centred */
            c->position = 0;
        }
    }
    if (!any) {
        set_timebase(as.tb_start);
        finish(dq, "Autoset: no signal");
        return;
    }

    const uint8_t src = pick_source();
    const trigger_source_t want = (src == 2u) ? TRIG_SRC_CH2 : TRIG_SRC_CH1;
    if (ss->trigger.source != want) {
        const trigger_source_t was = ss->trigger.source;
        ss->trigger.source = want;
        if (!fpga_apply_trigger_source())
            ss->trigger.source = was;
    }
    ss->trigger.mode = TRIG_AUTO;
    if (as.has[src - 1u]) {
        ss->trigger.level = autoset_trigger_level(as.mid[src - 1u]);
        (void)fpga_apply_trigger_level(ss->trigger.level);
    }

    char v[12], t[12], pb[32];
    scope_cal_range_label(src, chs(src)->vdiv_idx, v, sizeof(v));
    scope_timebase_label(ss->timebase_idx, t, sizeof(t));
    snprintf(pb, sizeof(pb), "Auto: CH%u %.8s %.8s", (unsigned)src, v, t);
    finish(dq, pb);
}

void scope_autoset_status(char *out, unsigned n)
{
    static const char *const names[] = { "off", "vert", "freq", "vert2" };
    snprintf(out, n, "phase %s iter %u fidx %u/%u hz %ld src %u tb 0x%02X | "
             "ch1 r%u pp %ldmV mid %d sig %u set %u | ch2 r%u pp %ldmV mid %d sig %u set %u | "
             "gen %lu wait %lu",
             names[as.phase < 4 ? as.phase : 0], (unsigned)as.iter, (unsigned)as.fidx,
             (unsigned)as.nfreq, (long)as.hz, (unsigned)as.src, (unsigned)ss_()->timebase_idx,
             (unsigned)chs(1)->vdiv_idx, (long)(as.vpp[0] * 1000.0f), (int)as.mid[0],
             (unsigned)as.has[0], (unsigned)as.settled[0],
             (unsigned)chs(2)->vdiv_idx, (long)(as.vpp[1] * 1000.0f), (int)as.mid[1],
             (unsigned)as.has[1], (unsigned)as.settled[1],
             (unsigned long)fpga_acq_frame_generation(), (unsigned long)as.gen_wait);
}

bool scope_autoset_busy(void)
{
    return as.phase != AS_OFF;
}

void scope_autoset_cancel(void)
{
    as.phase = AS_OFF;
}

bool scope_autoset_start(QueueHandle_t dq)
{
    scope_state_t *ss = ss_();
    if (current_mode != MODE_OSCILLOSCOPE)
        return false;
#ifdef FEATURE_FFT
    if (scope_view != SCOPE_VIEW_TIME)
        return false;
#endif
    memset(&as, 0, sizeof as);
    as.tb_start = ss->timebase_idx;
    ss->running = true;                 /* it needs captures */
    ss->trigger.mode = TRIG_AUTO;       /* ... whatever the level */
    for (uint8_t ch = 1u; ch <= 2u; ch++)
        if (ch_on(ch) && scope_cal_mv_per_count(ch, chs(ch)->vdiv_idx) <= 0.0f)
            set_range(ch, 7u);          /* start on a calibrated range */
    start_vert(AS_VERT);
    wait_fresh();
    scope_show_popup("Autoset...");
    uint8_t cmd = DCMD_SCOPE_LIVE;
    xQueueSend(dq, &cmd, 0);
    return true;
}

void scope_autoset_service(QueueHandle_t dq)
{
    if (as.phase == AS_OFF)
        return;
    bool away = (current_mode != MODE_OSCILLOSCOPE);
#ifdef FEATURE_FFT
    away = away || (scope_view != SCOPE_VIEW_TIME);
#endif
    if (away) {
        as.phase = AS_OFF;
        return;
    }
    const uint32_t g = fpga_acq_frame_generation();
    if ((int32_t)(g - as.gen_wait) < 0) {
        if ((int32_t)(xTaskGetTickCount() - as.deadline) >= 0)
            finish(dq, "Autoset: no capture");
        return;
    }

    switch (as.phase) {
    case AS_VERT:
        if (!vert_step()) {
            if (as.iter > 0u)
                wait_fresh();
            return;
        }
        as.src = pick_source();
        /* Nothing seen on either channel: look down to 5 kS/s only (a
         * signal slower than ~15 Hz that showed no swing at all is rare,
         * and 500 S/s costs 4 s of captures). */
        as.nfreq = (as.has[0] || as.has[1]) ? (uint8_t)sizeof freq_codes
                                            : (uint8_t)(sizeof freq_codes - 1u);
        as.fidx = 0u;
        as.phase = AS_FREQ;
        set_timebase(freq_codes[0]);
        wait_fresh();
        break;
    case AS_FREQ:
        freq_step();
        break;
    case AS_VERT2:
        if (!vert_step()) {
            if (as.iter > 0u)
                wait_fresh();
            return;
        }
        finish_ok(dq);
        break;
    default:
        as.phase = AS_OFF;
        break;
    }
}
