#pragma once

#include "can.h"
#include <stdbool.h>
#include <stddef.h>

// Maximum entries held in the ring buffer.
#define CAN_MON_LOG_SIZE  100
// Maximum characters per formatted line, including '\n' and the null terminator.
#define CAN_MON_LINE_MAX   96

/** Create the internal mutex. Must be called before any task that calls can_mon_push(). */
void can_mon_init(void);

/**
 * Push one frame into the ring buffer and schedule a UI refresh.
 * Safe to call from any FreeRTOS task or ISR-deferred context.
 *
 * @param is_tx   true  → frame was transmitted by this device on CAN
 * @param is_usb  true  → frame originated from the USB host (implies is_tx)
 */
void can_mon_push(const can_frame_t *f, bool is_tx, bool is_usb);

/**
 * Same as can_mon_push() but must be called only from within the LVGL task
 * (e.g. from ui_encoder_event). Skips display_lock() because the LVGL task
 * already holds it — calling the regular variant from there deadlocks.
 * Does not schedule a refresh; the caller is already in the LVGL task and
 * can refresh directly if needed.
 */
void can_mon_push_from_ui(const can_frame_t *f, bool is_tx, bool is_usb);

/** Push a visual separator line ("---...---") into the ring buffer. */
void can_mon_push_separator(void);

/**
 * Same as can_mon_push_separator() but must be called only from within the
 * LVGL task (e.g. from ui_encoder_event). Skips display_lock() because the
 * LVGL task already holds it — calling the regular variant from there deadlocks.
 */
void can_mon_push_separator_from_ui(void);

/**
 * Register the callback that fires (via lv_async_call) whenever new data arrives.
 * The callback runs in the LVGL task — LVGL APIs may be called freely inside it.
 */
void can_mon_set_notify(void (*fn)(void));

/**
 * Render the full log (oldest first) into out[0..out_len-1] as a null-terminated string.
 * Returns the number of bytes written (excluding the null terminator). Thread-safe.
 */
int can_mon_snapshot(char *out, size_t out_len);
