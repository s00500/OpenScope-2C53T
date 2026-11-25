/*
 * Host test for the display-task probe decoder (EXP-76).
 *
 * The decoder reads a FreeRTOS ARM_CM4F saved context off a task's stack.
 * If it picks the wrong words, the `display` shell command prints a
 * plausible-looking PC that points nowhere near where the task stopped --
 * the stable-wrong-number failure this project keeps meeting -- so the frame
 * offsets are pinned here for both frame shapes.
 *
 *   make test-display-trace
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "display_trace.h"

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define CODE_LO 0x08000000u
#define CODE_HI 0x08100000u

/* Build a saved context at the start of buf. Returns the index just past the
 * hardware frame (where the caller's stack continues). */
static size_t build(uint32_t *buf, uint32_t exc_return, uint32_t lr, uint32_t pc)
{
    size_t i = 0;
    for (int r = 4; r <= 11; r++)
        buf[i++] = 0xA0000000u | (uint32_t)r;          /* r4-r11 */
    buf[i++] = exc_return;
    const int fpu = (exc_return & 0x10u) == 0u;
    if (fpu)
        for (int k = 0; k < 16; k++)
            buf[i++] = 0x08001111u;                     /* s16-s31: code-like on purpose */
    buf[i++] = 0x00000000u;                             /* r0 */
    buf[i++] = 0x00000001u;                             /* r1 */
    buf[i++] = 0x08002223u;                             /* r2: code-like, must not count */
    buf[i++] = 0x00000003u;                             /* r3 */
    buf[i++] = 0x0000000Cu;                             /* r12 */
    buf[i++] = lr;
    buf[i++] = pc;
    buf[i++] = 0x01000000u;                             /* xpsr */
    if (fpu)
        for (int k = 0; k < 18; k++)
            buf[i++] = 0x08003333u;                     /* s0-s15, fpscr, reserved */
    return i;
}

static void test_plain_frame(void)
{
    uint32_t st[64];
    memset(st, 0, sizeof st);
    size_t i = build(st, 0xFFFFFFFDu, 0x08012345u, 0x08012350u);
    st[i++] = 0x20001234u;   /* RAM pointer */
    st[i++] = 0x0801AAABu;   /* return address */
    st[i++] = 0x0801AAAAu;   /* even: data, not a Thumb return address */
    st[i++] = 0x08200001u;   /* outside the code range */
    st[i++] = 0x08000101u;   /* return address */
    display_probe_t p;
    display_probe_decode(st, st + i, CODE_LO, CODE_HI, &p);
    CHECK(p.valid, "plain frame not decoded");
    CHECK(!p.fpu_frame, "plain frame taken for an FPU frame");
    CHECK(p.pc == 0x08012350u, "pc 0x%08X", (unsigned)p.pc);
    CHECK(p.lr == 0x08012345u, "lr 0x%08X", (unsigned)p.lr);
    CHECK(p.exc_return == 0xFFFFFFFDu, "exc_return 0x%08X", (unsigned)p.exc_return);
    CHECK(p.n_code == 2 && p.code[0] == 0x0801AAABu && p.code[1] == 0x08000101u,
          "code words: n=%u [0]=0x%08X", (unsigned)p.n_code, (unsigned)p.code[0]);
    CHECK(p.sp == (uint32_t)(uintptr_t)st, "sp not reported");
}

static void test_fpu_frame(void)
{
    uint32_t st[96];
    memset(st, 0, sizeof st);
    size_t i = build(st, 0xFFFFFFEDu, 0x08045671u, 0x08045678u);
    st[i++] = 0x0804ABCDu;
    display_probe_t p;
    display_probe_decode(st, st + i, CODE_LO, CODE_HI, &p);
    CHECK(p.valid, "FPU frame not decoded");
    CHECK(p.fpu_frame, "FPU frame not recognised");
    CHECK(p.pc == 0x08045678u, "FPU frame pc 0x%08X (offsets ignore s16-s31?)", (unsigned)p.pc);
    CHECK(p.lr == 0x08045671u, "FPU frame lr 0x%08X", (unsigned)p.lr);
    /* the extended frame (s0-s15...) must be skipped, not scanned */
    CHECK(p.n_code == 1 && p.code[0] == 0x0804ABCDu,
          "FPU frame code words: n=%u [0]=0x%08X", (unsigned)p.n_code, (unsigned)p.code[0]);
}

static void test_truncated_and_null(void)
{
    uint32_t st[64];
    memset(st, 0, sizeof st);
    size_t i = build(st, 0xFFFFFFEDu, 0x08000001u, 0x08000002u);
    display_probe_t p;
    display_probe_decode(st, st + i - 1, CODE_LO, CODE_HI, &p);
    CHECK(!p.valid, "a frame running past the stack top was decoded");
    display_probe_decode(st, st + 10, CODE_LO, CODE_HI, &p);
    CHECK(!p.valid, "a 10-word stack was decoded");
    display_probe_decode(NULL, NULL, CODE_LO, CODE_HI, &p);
    CHECK(!p.valid && p.n_code == 0, "NULL decoded");
}

static void test_scan_cap(void)
{
    uint32_t st[128];
    memset(st, 0, sizeof st);
    size_t i = build(st, 0xFFFFFFFDu, 0x08000001u, 0x08000002u);
    for (int k = 0; k < 40; k++)
        st[i++] = 0x08010001u + 2u * (uint32_t)k;
    display_probe_t p;
    display_probe_decode(st, st + i, CODE_LO, CODE_HI, &p);
    CHECK(p.n_code == DISPLAY_PROBE_CODE_MAX, "scan not capped: %u", (unsigned)p.n_code);
    CHECK(p.code[0] == 0x08010001u, "scan does not start nearest the frame");
}

static void test_stage_names(void)
{
    for (uint8_t s = 0; s < DT_STAGE_COUNT; s++)
        CHECK(strcmp(display_stage_name(s), "?") != 0 && display_stage_name(s)[0] != '\0',
              "stage %u has no name", (unsigned)s);
    CHECK(strcmp(display_stage_name(DT_STAGE_COUNT), "?") == 0, "out-of-range stage named");
    CHECK(strcmp(display_stage_name(DT_SCOPE_RENDER), "scope_render") == 0, "stage names out of order");
    CHECK(strcmp(display_stage_name(DT_POPUP), "popup") == 0, "stage names out of order");
}

/* EXP-78: the values read off the bench panel. */
static void test_panel_ok(void)
{
    CHECK(display_panel_ok(0x9C, 0xA0, 0xA0), "a configured panel (0x9C, A0) judged lost");
    CHECK(!display_panel_ok(0x08, 0x00, 0xA0), "the hardware-reset signature (0x08, 00) judged fine");
    CHECK(!display_panel_ok(0x08, 0xA0, 0xA0), "a software-reset panel (asleep, MADCTL kept) judged fine");
    CHECK(!display_panel_ok(0x9C, 0x00, 0xA0), "a panel that lost MADCTL judged fine");
    CHECK(!display_panel_ok(0x98, 0xA0, 0xA0), "display off judged fine");
    CHECK(strcmp(display_stage_name(DT_PANEL), "panel") == 0, "DT_PANEL has no name");
}

int main(void)
{
    test_plain_frame();
    test_fpu_frame();
    test_truncated_and_null();
    test_scan_cap();
    test_stage_names();
    test_panel_ok();
    if (failures) {
        printf("test_display_trace: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_display_trace: all passed\n");
    return 0;
}
