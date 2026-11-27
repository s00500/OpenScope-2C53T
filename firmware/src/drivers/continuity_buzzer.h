/*
 * OpenScope 2C53T - continuity buzzer drive.
 *
 * The buzzer is on PB9, bit-banged here (stock drives it from TMR11_CH1, #94).
 * The beep follows the meter SoC's BUZ tone on PC3, counted by EXINT3 as stock
 * does, so it starts within a few ms of contact; the decoded reading can mute
 * it above the user's threshold (continuity_veto.h) and is the fallback when
 * no tone has been seen. See continuity_buzzer.c and EXP-93.
 */
#ifndef CONTINUITY_BUZZER_H
#define CONTINUITY_BUZZER_H

#include <stdbool.h>
#include <stdint.h>

void continuity_buzzer_init(void);
void continuity_buzzer_create_task(void);
void continuity_buzzer_force_ms(uint32_t duration_ms);
void continuity_buzzer_snapshot(bool *task_started, bool *active,
                                uint32_t *toggle_count,
                                uint32_t *create_fail_count);

/* The PC3 detector and the reading's veto, for the shell's `buzzer status`.
 * Read without a lock: fields may be one sample apart. */
typedef struct {
    uint32_t pc3_edges;      /* EXINT3 rising edges since boot */
    bool     seen;           /* a tone has been seen this session */
    bool     is_short;       /* the tone (or its 105 ms hold) is on */
    bool     muted;          /* the reading mutes it (continuity_veto.h) */
    uint32_t onset_age_ms;   /* since the tone started; 0 when no tone */
    uint32_t veto_upd;       /* update_count of the last frame the veto saw */
} continuity_pc3_debug_t;

void continuity_buzzer_pc3_snapshot(continuity_pc3_debug_t *out);

#endif /* CONTINUITY_BUZZER_H */
