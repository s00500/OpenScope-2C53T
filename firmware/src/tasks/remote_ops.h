/*
 * OpenScope 2C53T - remote control surface (remote_protocol.md §10, M6)
 *
 * The binary protocol's SET_MODE / SET_PARAM / ACTION land on the USB shell
 * task, but every piece of UI state has ONE writer: the input task. These
 * functions hand each write to that task through a single-slot mailbox and
 * wait for the outcome, so a remote write runs exactly where a button press
 * runs -- same setters, same hardware entry points, same settings-persistence
 * tail -- and can never race one.
 *
 * Contract (esp_comm.h): ESP_APPLY_OK means the write took; anything else
 * means NOTHING changed, except ESP_APPLY_TIMED_OUT, which is the one honest
 * "may or may not have happened" answer.
 */

#ifndef REMOTE_OPS_H
#define REMOTE_OPS_H

#include <stdint.h>
#include "esp_comm.h"
#include "FreeRTOS.h"
#include "queue.h"

/* Wakes the input task through its own queue. The key scan produces ids
 * 1..15 only, so this value can never be a real press. */
#define BTN_REMOTE_OP   0x7Fu

/* Shell-task side: esp_comm providers (usb_debug.c binds them). */
esp_apply_result_t remote_set_mode(uint8_t mode);
esp_apply_result_t remote_set_param(uint8_t key, const esp_value_t *v);
esp_apply_result_t remote_action(uint8_t action, uint8_t arg);
/* Read-only snapshot of every key; safe from any task (reads only). */
void remote_state(esp_state_writer_t *w);

/* Input-task side: called by vInputTask when it dequeues BTN_REMOTE_OP. */
void remote_ops_service(QueueHandle_t display_queue);

#endif /* REMOTE_OPS_H */
