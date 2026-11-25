/*
 * OpenScope 2C53T - remote control surface (remote_protocol.md §10, M6)
 *
 * See remote_ops.h for the why. The mailbox:
 *
 *   shell task                         input task
 *   ----------                         ----------
 *   fill s_op, phase = PENDING
 *   post BTN_REMOTE_OP  ------------>  dequeue it
 *   wait (task notification)           PENDING -> RUNNING (critical section)
 *                                      apply, persistence tail, redraw
 *                         <----------  RUNNING -> DONE, notify
 *   take result, phase = IDLE
 *
 * The shell task is the only submitter (esp_comm dispatches on it), so one
 * slot is enough. If the input task has not picked the op up by the pickup
 * deadline, the submitter cancels it (PENDING -> IDLE, atomically against
 * the pickup) and reports BUSY: provably nothing changed. An op that has
 * STARTED is waited for; one that overruns even the run cap is reported as
 * TIMED_OUT ("may have taken") and its late completion is discarded.
 */

#include "remote_ops.h"
#include "input_handler.h"
#include "ui.h"
#include "scope_state.h"
#include "scope_cal.h"
#include "../ui/scope_timebase.h"
#include "signal_gen.h"
#include "theme.h"
#include "math_channel.h"
#include "persistence.h"
#include "fpga.h"
#include "settings_store.h"
#include "task.h"
#include <string.h>

/* Pickup: the input queue is 15 deep and one press can take ~1 s (an acq
 * park), so a backlog behind real presses is possible; 3 s covers a few. */
#define REMOTE_PICKUP_MS    3000u
/* Run cap: the slowest setter parks the acq task (<= 1 s). */
#define REMOTE_RUN_CAP_MS   8000u

enum { OP_IDLE = 0, OP_PENDING, OP_RUNNING, OP_DONE };
enum { KIND_MODE = 1, KIND_PARAM, KIND_ACTION };

static struct {
    volatile uint8_t   phase;
    volatile uint8_t   orphaned;    /* submitter gave up while RUNNING */
    uint8_t            kind;
    uint8_t            key;         /* mode / key / action */
    uint8_t            arg;
    esp_value_t        v;
    esp_apply_result_t result;
    TaskHandle_t       waiter;
} s_op;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ─── shell-task side ─────────────────────────────────────────────── */

static esp_apply_result_t submit(uint8_t kind, uint8_t key, uint8_t arg, const esp_value_t *v)
{
    uint8_t phase;
    esp_apply_result_t r;

    taskENTER_CRITICAL();
    if (s_op.phase == OP_DONE && s_op.orphaned)
        s_op.phase = OP_IDLE;               /* late completion of a TIMED_OUT op */
    phase = s_op.phase;
    taskEXIT_CRITICAL();
    if (phase != OP_IDLE)
        return ESP_APPLY_BUSY;              /* an earlier op is still running */

    s_op.kind     = kind;
    s_op.key      = key;
    s_op.arg      = arg;
    s_op.v        = v ? *v : (esp_value_t){ 0, 0.0f };
    s_op.result   = ESP_APPLY_UNKNOWN;
    s_op.orphaned = 0;
    s_op.waiter   = xTaskGetCurrentTaskHandle();
    (void)ulTaskNotifyTake(pdTRUE, 0);      /* drop a stale notification */

    taskENTER_CRITICAL();
    s_op.phase = OP_PENDING;
    taskEXIT_CRITICAL();
    if (!input_inject_button((button_id_t)BTN_REMOTE_OP)) {
        s_op.phase = OP_IDLE;               /* never queued: nothing can run it */
        return ESP_APPLY_BUSY;
    }

    const uint32_t t0 = now_ms();
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        const uint32_t dt = now_ms() - t0;

        taskENTER_CRITICAL();
        phase = s_op.phase;
        if (phase == OP_DONE) {
            r = s_op.result;
            s_op.phase = OP_IDLE;
            taskEXIT_CRITICAL();
            return r;
        }
        if (phase == OP_PENDING && dt >= REMOTE_PICKUP_MS) {
            s_op.phase = OP_IDLE;           /* cancelled before pickup: unchanged */
            taskEXIT_CRITICAL();
            return ESP_APPLY_BUSY;
        }
        if (phase == OP_RUNNING && dt >= REMOTE_RUN_CAP_MS) {
            s_op.orphaned = 1;              /* it may still take: say so */
            taskEXIT_CRITICAL();
            return ESP_APPLY_TIMED_OUT;
        }
        taskEXIT_CRITICAL();
    }
}

esp_apply_result_t remote_set_mode(uint8_t mode)
{
    return submit(KIND_MODE, mode, 0, NULL);
}

esp_apply_result_t remote_set_param(uint8_t key, const esp_value_t *v)
{
    return submit(KIND_PARAM, key, 0, v);
}

esp_apply_result_t remote_action(uint8_t action, uint8_t arg)
{
    return submit(KIND_ACTION, action, arg, NULL);
}

/* ─── state snapshot (any task: reads only) ───────────────────────── */

static uint32_t round_u32(float v)
{
    return (v > 0.0f) ? (uint32_t)(v + 0.5f) : 0u;
}

void remote_state(esp_state_writer_t *w)
{
    const scope_state_t *ss = scope_state_get();
    const siggen_config_t *sg = siggen_get_config();
    const uint8_t code = fpga_acq_rate_idx_get();

    esp_state_put_int(w, ESP_KEY_MODE,    (int32_t)current_mode);
    esp_state_put_int(w, ESP_KEY_THEME,   (int32_t)theme_get_id());
    esp_state_put_int(w, ESP_KEY_STARTUP, (int32_t)startup_mode);
    esp_state_put_int(w, ESP_KEY_MATH,    math_enabled ? 1 : 0);
    esp_state_put_int(w, ESP_KEY_MATH_OP, (int32_t)math_op);
    esp_state_put_int(w, ESP_KEY_PERSIST, persist_enabled ? 1 : 0);
    /* Even = a committed record; odd = mid-commit, reported as the previous
     * one so a host never fetches on a half-written generation. */
    {
        const uint32_t g = fpga_acq_frame_generation();
        esp_state_put_int(w, ESP_KEY_FRAME_GEN,
                          (current_mode == MODE_OSCILLOSCOPE && fpga_data_ready())
                              ? (int32_t)(g & ~1u) : 0);
    }

    esp_state_put_int(w, ESP_KEY_TIMEBASE,    (int32_t)ss->timebase_idx);
    esp_state_put_int(w, ESP_KEY_TIMEBASE_HW, (int32_t)code);
    /* Same rule as WAVEFORM_FRAME: a rate is claimed only when the display
     * and the hardware agree on the code, and only with a tier behind it. */
    {
        const scope_tb_tier_t tier = scope_timebase_get_tier(code);
        const bool agree = (code == ss->timebase_idx);
        esp_state_put_int(w, ESP_KEY_SAMPLE_RATE,
                          (agree && tier != SCOPE_TB_NONE)
                              ? (int32_t)round_u32(scope_timebase_sample_rate(code)) : 0);
        esp_state_put_int(w, ESP_KEY_RATE_TIER, agree ? (int32_t)tier : (int32_t)SCOPE_TB_NONE);
    }
    esp_state_put_int(w, ESP_KEY_RUNNING,   ss->running ? 1 : 0);
#ifdef FEATURE_FFT
    esp_state_put_int(w, ESP_KEY_VIEW,      (int32_t)scope_view);
#endif
    esp_state_put_int(w, ESP_KEY_GRATICULE, ss->true_scale ? 1 : 0);
    esp_state_put_int(w, ESP_KEY_SOFTTRIG,  ss->soft_trigger ? 1 : 0);
    esp_state_put_int(w, ESP_KEY_COUNTS_PER_DIV, (int32_t)SCOPE_CAL_COUNTS_PER_DIV);
    esp_state_put_int(w, ESP_KEY_TRIG_MODE,   (int32_t)ss->trigger.mode);
    esp_state_put_int(w, ESP_KEY_TRIG_EDGE,   (int32_t)ss->trigger.edge);
    esp_state_put_int(w, ESP_KEY_TRIG_SOURCE, (int32_t)ss->trigger.source);
    esp_state_put_int(w, ESP_KEY_TRIG_LEVEL,  (int32_t)ss->trigger.level);
    esp_state_put_int(w, ESP_KEY_TRIG_X,      (int32_t)ss->trig_x);
    esp_state_put_int(w, ESP_KEY_TRIG_CODE,   (int32_t)fpga_acq_trig_code_get());
    esp_state_put_int(w, ESP_KEY_ACTIVE_CH,   (int32_t)active_channel);

    for (uint8_t c = 0; c < 2u; c++) {
        const channel_state_t *ch = c ? &ss->ch2 : &ss->ch1;
        const uint8_t base = ESP_KEY_CH_BASE(c);
        const scope_cal_tier_t tier = scope_cal_get_tier((uint8_t)(c + 1u), ch->vdiv_idx);
        esp_state_put_int(w, (uint8_t)(base + ESP_KEY_CH_ENABLED),  ch->enabled ? 1 : 0);
        esp_state_put_int(w, (uint8_t)(base + ESP_KEY_CH_VDIV),     (int32_t)ch->vdiv_idx);
        esp_state_put_int(w, (uint8_t)(base + ESP_KEY_CH_COUPLING), (int32_t)ch->coupling);
        esp_state_put_int(w, (uint8_t)(base + ESP_KEY_CH_PROBE),    (int32_t)ch->probe);
        esp_state_put_int(w, (uint8_t)(base + ESP_KEY_CH_UV_PER_DIV),
                          tier != SCOPE_CAL_NONE
                              ? (int32_t)round_u32(scope_cal_volts_per_div((uint8_t)(c + 1u),
                                                                           ch->vdiv_idx) * 1e6f)
                              : 0);
        esp_state_put_int(w, (uint8_t)(base + ESP_KEY_CH_VDIV_TIER), (int32_t)tier);
    }

    esp_state_put_int(w, ESP_KEY_METER_FUNCTION, (int32_t)meter_submode);
    esp_state_put_int(w, ESP_KEY_METER_LAYOUT,   (int32_t)meter_layout);
    esp_state_put_int(w, ESP_KEY_METER_REL,      meter_rel_enabled ? 1 : 0);
    esp_state_put_int(w, ESP_KEY_METER_HOLD,     meter_hold_enabled ? 1 : 0);

    /* The generator is stock's (DAC2/PA5, TMR7, DMA2 ch4): integer hertz,
     * amplitude in 0.1 V steps, no offset (its output is 0..amplitude). The
     * wire types are kept so older hosts still parse the reply. */
    esp_state_put_int(w, ESP_KEY_SG_WAVE,   (int32_t)sg->waveform);
    esp_state_put_f32(w, ESP_KEY_SG_FREQ,   (float)sg->frequency_hz);
    esp_state_put_f32(w, ESP_KEY_SG_AMP,    (float)sg->amplitude_dv / 10.0f);
    esp_state_put_f32(w, ESP_KEY_SG_OFFSET, 0.0f);
    esp_state_put_int(w, ESP_KEY_SG_DUTY,   (int32_t)sg->duty_pct);
    esp_state_put_int(w, ESP_KEY_SG_OUTPUT, sg->output_enabled ? 1 : 0);
    /* What TMR7 really produces; EXP-79 found it exact against a Rigol's
     * counter (to the crystal offset), so a host can use it as the truth. */
    siggen_timing_t sgt;
    esp_state_put_f32(w, ESP_KEY_SG_FREQ_ACTUAL,
                      siggen_get_timing(&sgt) ? (float)sgt.actual_mhz / 1000.0f : 0.0f);
}

/* ─── input-task side ─────────────────────────────────────────────── */

static bool is_bool(int32_t i)        { return i == 0 || i == 1; }
static bool in_range(int32_t i, int32_t lo, int32_t hi) { return i >= lo && i <= hi; }

static esp_apply_result_t apply_mode(uint8_t mode)
{
    if (mode >= (uint8_t)MODE_COUNT)
        return ESP_APPLY_BAD_ARG;
    input_enter_mode((device_mode_t)mode);
    return ESP_APPLY_OK;
}

/* Scope keys that drive hardware (relays, FPGA registers, the FFT pool) are
 * accepted in scope mode only, as on the buttons; pure scope settings are
 * accepted anywhere. Meter and siggen keys need their own mode. */
static esp_apply_result_t apply_channel(uint8_t c, uint8_t off, int32_t i)
{
    scope_state_t *ss = scope_state_get();
    channel_state_t *ch = c ? &ss->ch2 : &ss->ch1;

    switch (off) {
    case ESP_KEY_CH_ENABLED:
        if (!is_bool(i)) return ESP_APPLY_BAD_ARG;
        ch->enabled = (i != 0);
        return ESP_APPLY_OK;
    case ESP_KEY_CH_PROBE:
        if (!in_range(i, 0, PROBE_COUNT - 1)) return ESP_APPLY_BAD_ARG;
        ch->probe = (probe_t)i;
        return ESP_APPLY_OK;
    case ESP_KEY_CH_VDIV:
        if (!in_range(i, 0, VDIV_COUNT - 1)) return ESP_APPLY_BAD_ARG;
        if (current_mode != MODE_OSCILLOSCOPE) return ESP_APPLY_WRONG_MODE;
        /* Relays first, label after: the label never claims a range the
         * frontend is not in (EXP-19). */
        if (!fpga_apply_vdiv((uint8_t)(c + 1u), (uint8_t)i)) return ESP_APPLY_HW_REFUSED;
        ch->vdiv_idx = (uint8_t)i;
        return ESP_APPLY_OK;
    case ESP_KEY_CH_COUPLING:
        if (i == (int32_t)COUPLING_GND) return ESP_APPLY_UNSUPPORTED;   /* no ground path known */
        if (i != (int32_t)COUPLING_DC && i != (int32_t)COUPLING_AC) return ESP_APPLY_BAD_ARG;
        if (current_mode != MODE_OSCILLOSCOPE) return ESP_APPLY_WRONG_MODE;
        return input_apply_coupling(c, (coupling_t)i) ? ESP_APPLY_OK : ESP_APPLY_HW_REFUSED;
    default:
        return ESP_APPLY_UNKNOWN;           /* RO offsets never get here (esp_comm checks) */
    }
}

static esp_apply_result_t apply_param(uint8_t key, const esp_value_t *v)
{
    scope_state_t *ss = scope_state_get();
    const int32_t i = v->i;

    if (key >= ESP_KEY_CH_BASE(0) && key < (uint8_t)(ESP_KEY_CH_BASE(1) + 8u)) {
        const uint8_t c = (key >= ESP_KEY_CH_BASE(1)) ? 1u : 0u;
        return apply_channel(c, (uint8_t)(key - ESP_KEY_CH_BASE(c)), i);
    }

    switch (key) {
    /* -- system ---------------------------------------------------- */
    case ESP_KEY_THEME:
        if (!in_range(i, 0, THEME_COUNT - 1)) return ESP_APPLY_BAD_ARG;
        theme_set((theme_id_t)i);
        return ESP_APPLY_OK;
    case ESP_KEY_STARTUP:
        if (!in_range(i, 0, STARTUP_COUNT - 1)) return ESP_APPLY_BAD_ARG;
        startup_mode_set((startup_mode_t)i);
        return ESP_APPLY_OK;
    case ESP_KEY_MATH:
        if (!is_bool(i)) return ESP_APPLY_BAD_ARG;
        math_enabled = (i != 0);
        return ESP_APPLY_OK;
    case ESP_KEY_MATH_OP:
        if (!in_range(i, 0, MATH_COUNT - 1)) return ESP_APPLY_BAD_ARG;
        math_op = (uint8_t)i;
        return ESP_APPLY_OK;
    case ESP_KEY_PERSIST:
        if (!is_bool(i)) return ESP_APPLY_BAD_ARG;
        input_set_persist(i != 0);
        return ESP_APPLY_OK;

    /* -- scope: hardware ------------------------------------------- */
    case ESP_KEY_TIMEBASE:
        if (!in_range(i, 0, TIMEBASE_COUNT - 1)) return ESP_APPLY_BAD_ARG;
        if (current_mode != MODE_OSCILLOSCOPE) return ESP_APPLY_WRONG_MODE;
        /* Through the one writer of reg 0x01; the display follows only if
         * it took (EXP-17: never relabel an axis the hardware is not on). */
        if (!fpga_apply_timebase((uint8_t)i)) return ESP_APPLY_HW_REFUSED;
        ss->timebase_idx = (uint8_t)i;
        return ESP_APPLY_OK;
    case ESP_KEY_TRIG_LEVEL:
        if (!in_range(i, -100, 100)) return ESP_APPLY_BAD_ARG;
        if (current_mode != MODE_OSCILLOSCOPE) return ESP_APPLY_WRONG_MODE;
        if (!fpga_apply_trigger_level((int)i)) return ESP_APPLY_HW_REFUSED;
        ss->trigger.level = (int16_t)i;
        return ESP_APPLY_OK;
    case ESP_KEY_VIEW:
#ifdef FEATURE_FFT
        if (!in_range(i, 0, SCOPE_VIEW_COUNT - 1)) return ESP_APPLY_BAD_ARG;
        if (current_mode != MODE_OSCILLOSCOPE) return ESP_APPLY_WRONG_MODE;
        input_set_scope_view((scope_view_t)i);
        return ESP_APPLY_OK;
#else
        return ESP_APPLY_UNSUPPORTED;
#endif

    /* -- scope: pure settings -------------------------------------- */
    case ESP_KEY_RUNNING:
        if (!is_bool(i)) return ESP_APPLY_BAD_ARG;
        ss->running = (i != 0);
        return ESP_APPLY_OK;
    case ESP_KEY_GRATICULE:
        if (!is_bool(i)) return ESP_APPLY_BAD_ARG;
        ss->true_scale = (i != 0);
        return ESP_APPLY_OK;
    case ESP_KEY_SOFTTRIG:
        if (!is_bool(i)) return ESP_APPLY_BAD_ARG;
        ss->soft_trigger = (i != 0);
        return ESP_APPLY_OK;
    case ESP_KEY_TRIG_MODE:
        if (!in_range(i, 0, TRIG_COUNT - 1)) return ESP_APPLY_BAD_ARG;
        ss->trigger.mode = (trigger_mode_t)i;
        return ESP_APPLY_OK;
    case ESP_KEY_TRIG_EDGE:
        if (!in_range(i, 0, TRIG_EDGE_COUNT - 1)) return ESP_APPLY_BAD_ARG;
        ss->trigger.edge = (trigger_edge_t)i;
        return ESP_APPLY_OK;
    case ESP_KEY_TRIG_SOURCE:
        if (!in_range(i, 0, TRIG_SRC_COUNT - 1)) return ESP_APPLY_BAD_ARG;
        ss->trigger.source = (trigger_source_t)i;
        return ESP_APPLY_OK;
    case ESP_KEY_TRIG_X:
        if (!in_range(i, 8, 312)) return ESP_APPLY_BAD_ARG;
        ss->trig_x = (int16_t)i;
        return ESP_APPLY_OK;
    case ESP_KEY_ACTIVE_CH:
        if (!is_bool(i)) return ESP_APPLY_BAD_ARG;
        active_channel = (uint8_t)i;
        return ESP_APPLY_OK;

    /* -- meter ------------------------------------------------------ */
    case ESP_KEY_METER_FUNCTION:
        if (!in_range(i, 0, METER_SUBMODE_COUNT - 1)) return ESP_APPLY_BAD_ARG;
        if (current_mode != MODE_MULTIMETER) return ESP_APPLY_WRONG_MODE;
        meter_function_select((uint8_t)i);
        return ESP_APPLY_OK;
    case ESP_KEY_METER_LAYOUT:
        if (!in_range(i, 0, METER_LAYOUT_COUNT - 1)) return ESP_APPLY_BAD_ARG;
        if (current_mode != MODE_MULTIMETER) return ESP_APPLY_WRONG_MODE;
        meter_layout_select((uint8_t)i);
        return ESP_APPLY_OK;
    case ESP_KEY_METER_REL:
        if (!is_bool(i)) return ESP_APPLY_BAD_ARG;
        if (current_mode != MODE_MULTIMETER) return ESP_APPLY_WRONG_MODE;
        if ((i != 0) != meter_rel_enabled) meter_toggle_relative();
        return ESP_APPLY_OK;
    case ESP_KEY_METER_HOLD:
        if (!is_bool(i)) return ESP_APPLY_BAD_ARG;
        if (current_mode != MODE_MULTIMETER) return ESP_APPLY_WRONG_MODE;
        if ((i != 0) != meter_hold_enabled) meter_hold_toggle_now();
        /* Hold refuses with no live reading to freeze: report it. */
        return ((i != 0) == meter_hold_enabled) ? ESP_APPLY_OK : ESP_APPLY_HW_REFUSED;

    /* -- signal generator ------------------------------------------ */
    case ESP_KEY_SG_WAVE:
    case ESP_KEY_SG_FREQ:
    case ESP_KEY_SG_AMP:
    case ESP_KEY_SG_OFFSET:
    case ESP_KEY_SG_DUTY:
    case ESP_KEY_SG_OUTPUT:
        if (current_mode != MODE_SIGNAL_GEN) return ESP_APPLY_WRONG_MODE;
        switch (key) {
        case ESP_KEY_SG_WAVE:
            if (!in_range(i, 0, SIGGEN_WAVEFORM_COUNT - 1)) return ESP_APPLY_BAD_ARG;
            siggen_set_waveform((uint8_t)i);
            return ESP_APPLY_OK;
        /* Out-of-range floats are clamped, like the buttons' presets; the
         * STATE reply shows the value that took. Frequency is held at 1 Hz
         * or more here: stock's 0 Hz means "output off", and over the wire
         * that is sg.output's job. */
        case ESP_KEY_SG_FREQ: {
            float f = v->f;
            if (f < 1.0f) f = 1.0f;
            if (f > (float)SIGGEN_FREQ_MAX_HZ) f = (float)SIGGEN_FREQ_MAX_HZ;
            siggen_set_frequency((uint32_t)(f + 0.5f));
            return ESP_APPLY_OK;
        }
        case ESP_KEY_SG_AMP: {
            float dv = v->f * 10.0f;
            if (dv < 0.0f) dv = 0.0f;
            if (dv > (float)SIGGEN_AMP_MAX_DV) dv = (float)SIGGEN_AMP_MAX_DV;
            siggen_set_amplitude_dv((uint8_t)(dv + 0.5f));
            return ESP_APPLY_OK;
        }
        case ESP_KEY_SG_DUTY:
            if (!in_range(i, 0, (int32_t)SIGGEN_DUTY_MAX)) return ESP_APPLY_BAD_ARG;
            siggen_set_duty((uint8_t)i);
            return ESP_APPLY_OK;
        case ESP_KEY_SG_OUTPUT:
            if (!is_bool(i)) return ESP_APPLY_BAD_ARG;
            siggen_enable(i != 0);
            return ESP_APPLY_OK;
        default: /* ESP_KEY_SG_OFFSET: read-only since the generator moved
                  * to stock's DAC2 path, which has no offset; the key table
                  * refuses the write before it gets here. */
            return ESP_APPLY_UNSUPPORTED;
        }

    default:
        return ESP_APPLY_UNKNOWN;
    }
}

static esp_apply_result_t apply_action(uint8_t action, uint8_t arg)
{
    (void)arg;
    switch (action) {
    case ESP_ACT_METER_RESET:
        if (current_mode != MODE_MULTIMETER) return ESP_APPLY_WRONG_MODE;
        meter_reset_minmaxavg();
        return ESP_APPLY_OK;
    case ESP_ACT_SETTINGS_SAVE:
        /* false only means nothing was pending, which is not a failure. */
        (void)settings_store_flush(now_ms());
        return ESP_APPLY_OK;
    default:
        return ESP_APPLY_UNKNOWN;
    }
}

void remote_ops_service(QueueHandle_t display_queue)
{
    esp_apply_result_t r;
    uint8_t cmd = DCMD_REDRAW_ALL;

    taskENTER_CRITICAL();
    if (s_op.phase != OP_PENDING) {         /* cancelled by the submitter, or stray */
        taskEXIT_CRITICAL();
        return;
    }
    s_op.phase = OP_RUNNING;
    taskEXIT_CRITICAL();

    switch (s_op.kind) {
    case KIND_MODE:   r = apply_mode(s_op.key); break;
    case KIND_PARAM:  r = apply_param(s_op.key, &s_op.v); break;
    case KIND_ACTION: r = apply_action(s_op.key, s_op.arg); break;
    default:          r = ESP_APPLY_UNKNOWN; break;
    }

    if (r == ESP_APPLY_OK) {
        /* The same tail a button press gets: persistence, then a repaint. */
        input_settings_tick();
        (void)xQueueSend(display_queue, &cmd, 0);
    }

    taskENTER_CRITICAL();
    s_op.result = r;
    s_op.phase  = OP_DONE;
    taskEXIT_CRITICAL();
    if (s_op.waiter)
        xTaskNotifyGive(s_op.waiter);
}
