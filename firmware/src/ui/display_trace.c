/*
 * Display-task breadcrumbs: stage names and the saved-context decoder.
 * See display_trace.h (EXP-76).
 */
#include "display_trace.h"

#include <stddef.h>
#include <string.h>

static const char *const stage_names[DT_STAGE_COUNT] = {
    "start", "mode_switch", "queue_wait", "cmd", "health", "fpga_service",
    "info_bar", "mask_slot", "heartbeat", "gate", "scope_render", "meter",
    "popup", "panel",
};

const char *display_stage_name(uint8_t stage)
{
    return stage < DT_STAGE_COUNT ? stage_names[stage] : "?";
}

bool display_panel_ok(uint8_t power, uint8_t madctl, uint8_t madctl_boot)
{
    return (power & 0x14u) == 0x14u && madctl == madctl_boot;
}

/* FreeRTOS GCC/ARM_CM4F xPortPendSVHandler saves, from pxTopOfStack up:
 *   r4-r11, r14 (EXC_RETURN)          9 words, always
 *   s16-s31                          16 words, if EXC_RETURN bit 4 is clear
 * and below that sits the frame the hardware stacked on exception entry:
 *   r0 r1 r2 r3 r12 lr pc xpsr        8 words
 *   s0-s15, fpscr, reserved          18 words, extended frame only. */
#define SW_WORDS      9u
#define SW_FPU_WORDS  16u
#define HW_WORDS      8u
#define HW_FPU_WORDS  18u

void display_probe_decode(const uint32_t *sp, const uint32_t *top,
                          uint32_t code_lo, uint32_t code_hi,
                          display_probe_t *out)
{
    memset(out, 0, sizeof *out);
    out->sp = (uint32_t)(uintptr_t)sp;
    if (sp == NULL || top == NULL || sp >= top ||
        (size_t)(top - sp) < SW_WORDS + HW_WORDS)
        return;

    out->exc_return = sp[8];
    out->fpu_frame = (out->exc_return & 0x10u) == 0u;
    const uint32_t *hw = sp + SW_WORDS + (out->fpu_frame ? SW_FPU_WORDS : 0u);
    const uint32_t *above = hw + HW_WORDS + (out->fpu_frame ? HW_FPU_WORDS : 0u);
    if (above > top)
        return;

    out->lr = hw[5];
    out->pc = hw[6];
    out->valid = true;

    for (const uint32_t *w = above; w < top && out->n_code < DISPLAY_PROBE_CODE_MAX; w++) {
        uint32_t v = *w;
        if ((v & 1u) && v >= code_lo && v < code_hi)
            out->code[out->n_code++] = v;
    }
}
