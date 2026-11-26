/*
 * OpenScope 2C53T: the autoset run (AUTO in the scope's time view).
 *
 * Applies the decisions of ui/autoset.c to the hardware and waits for fresh
 * captures between steps. It never blocks: scope_autoset_service() is called
 * from the input task's loop (every 100 ms at most) and does one step when a
 * capture taken at the current settings has arrived, so the task keeps
 * checking in with the health monitor (2 s deadline) and keys keep working.
 *
 * Phases: VERT (range per channel, until each settles) -> FREQ (cycles of the
 * source channel at 5 M / 500 k / 50 k / 5 k / 500 S/s, fast to slow so a
 * fast signal is never counted aliased) -> VERT again at the chosen timebase
 * -> finish (positions, trigger source and level, AUTO mode, a popup).
 * Any key cancels it, leaving the settings where it got to.
 */
#ifndef SCOPE_AUTOSET_H
#define SCOPE_AUTOSET_H

#include <stdbool.h>
#include "FreeRTOS.h"
#include "queue.h"

bool scope_autoset_start(QueueHandle_t dq);   /* false: not in the time view */
bool scope_autoset_busy(void);
void scope_autoset_cancel(void);
void scope_autoset_service(QueueHandle_t dq);

/* One line of state for the shell (`fpga scope autoset`): phase, step,
 * timebase index, frequency, source, per-channel range/pp/mid/signal, and
 * the generation it waits for. */
void scope_autoset_status(char *out, unsigned n);

#endif /* SCOPE_AUTOSET_H */
