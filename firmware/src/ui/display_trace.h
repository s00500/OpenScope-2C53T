/*
 * Display-task breadcrumbs (EXP-76, 2026-11-25).
 *
 * EXP-76 caught the display task stopped while acquisition, the input task
 * and USB all kept running: the LCD shadow stayed byte-identical through new
 * captures, a forced full redraw and a switch to meter mode. Guest builds do
 * not arm the watchdog, so nothing reset it and nothing said where it stood.
 *
 * Two instruments, both readable from the shell (`display`) while the task
 * itself is stuck:
 *   - a breadcrumb: which part of the display loop the task is in, since
 *     when, and how many passes it has completed;
 *   - its saved context: a task that is not running has its registers on its
 *     own stack (FreeRTOS ARM_CM4F PendSV layout), so another task can read
 *     the PC and LR it will resume at, plus the return-address-like words
 *     further up its stack (a heuristic call chain for addr2line).
 *
 * The decoder is pure (no FreeRTOS) so the host test can check it.
 */
#ifndef DISPLAY_TRACE_H
#define DISPLAY_TRACE_H

#include <stdbool.h>
#include <stdint.h>

/* Where the display loop is. Order matches the loop body. */
typedef enum {
    DT_START = 0,      /* initial draw, before the loop */
    DT_MODE_SWITCH,    /* mode-transition bookkeeping at the top of a pass */
    DT_QUEUE_WAIT,     /* xQueueReceive on the display queue (50 ms) */
    DT_CMD,            /* handling a DCMD (display_trace.cmd) */
    DT_HEALTH,
    DT_FPGA_SERVICE,   /* fpga_service_requests() */
    DT_INFO_BAR,
    DT_MASK_SLOT,
    DT_HEARTBEAT,      /* fpga_scope_heartbeat() */
    DT_GATE,           /* redraw_gate_step() */
    DT_SCOPE_RENDER,   /* scope_render() (display_trace.render_act) */
    DT_METER,          /* draw_meter_screen() from the periodic path */
    DT_POPUP,          /* ui_popup_overlay_service() and its repaint */
    DT_PANEL,          /* panel health check / re-init (EXP-78) */
    DT_STAGE_COUNT
} display_stage_t;

typedef struct {
    volatile uint32_t loops;       /* completed passes of the display loop */
    volatile uint32_t pass_tick;   /* tick at which the current pass began */
    volatile uint32_t stage_tick;  /* tick at which the current stage began */
    volatile uint8_t  stage;       /* display_stage_t */
    volatile uint8_t  cmd;         /* DCMD of the last DT_CMD */
    volatile uint8_t  render_act;  /* redraw_action_t of the last scope_render */
    volatile uint32_t panel_checks;     /* EXP-78: panel health reads */
    volatile uint32_t panel_recoveries; /* panel found reset and re-initialised */
    volatile uint8_t  panel_power;      /* RDDPM / MADCTL of the last bad read */
    volatile uint8_t  panel_madctl;
} display_trace_t;

const char *display_stage_name(uint8_t stage);

/* EXP-78: is the panel still configured? RDDPM must say awake (bit 4) and
 * display on (bit 2), and MADCTL must still hold what boot wrote: a hardware
 * reset (a physical press of UP on bench unit #1) reads power 0x08, MADCTL
 * 0x00, COLMOD 0x06. */
bool display_panel_ok(uint8_t power, uint8_t madctl, uint8_t madctl_boot);

/* What a not-running task's saved context says. */
#define DISPLAY_PROBE_CODE_MAX 12
typedef struct {
    bool     valid;        /* false: the saved SP or frame fell outside the stack */
    bool     fpu_frame;    /* EXC_RETURN bit 4 clear: s16-s31 and the extended frame saved */
    uint32_t sp;           /* saved top of stack (pxTopOfStack) */
    uint32_t exc_return;
    uint32_t pc, lr;       /* from the hardware-stacked frame */
    uint8_t  n_code;       /* code-like words found above the frame */
    uint32_t code[DISPLAY_PROBE_CODE_MAX];
} display_probe_t;

/* Decode a FreeRTOS ARM_CM4F saved context.
 *   sp:  the task's pxTopOfStack (lowest live address)
 *   top: one past the highest address of the task's stack
 *   code_lo/code_hi: address range that counts as code for the stack scan
 * A code-like word has the Thumb bit set and lies in [code_lo, code_hi). */
void display_probe_decode(const uint32_t *sp, const uint32_t *top,
                          uint32_t code_lo, uint32_t code_hi,
                          display_probe_t *out);

/* Firmware only (main.c): the breadcrumb and a probe of the display task.
 * state is the FreeRTOS eTaskState (0 running, 1 ready, 2 blocked,
 * 3 suspended, 4 deleted), 0xFF if unavailable. */
extern display_trace_t display_trace;
bool display_task_probe(display_probe_t *p, uint8_t *state, uint32_t *stack_free_words);
/* EXP-78: suspend / resume the display task so another task can own the LCD
 * bus, and ask it for a full repaint afterwards. */
void display_task_hold(bool hold);
void display_request_redraw(void);

#endif /* DISPLAY_TRACE_H */
