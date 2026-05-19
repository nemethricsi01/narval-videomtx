#pragma once

#include <stdint.h>

#define VIDEOMTX_SIZE  16   /* 16 outputs, each routed to one of 16 inputs */

/**
 * Callback invoked (in the caller's task context) whenever videomtx_set()
 * changes a crosspoint.  Register once from the UI task before any other task
 * can call videomtx_set().
 */
typedef void (*videomtx_notify_fn_t)(uint8_t output, uint8_t input);

/** Initialise routing table to identity (out N → in N). */
void     videomtx_init(void);

/**
 * Set a single crosspoint and trigger the notify callback.
 * Use from external tasks (e.g. CAN handler) so the display refreshes.
 */
void     videomtx_set(uint8_t output, uint8_t input);

/**
 * Same as videomtx_set() but skips the notify callback.
 * Use from the LVGL task (UI edits) to avoid re-entering the display lock.
 */
void     videomtx_set_silent(uint8_t output, uint8_t input);

/** Return the currently routed input for the given output (0-based). */
uint8_t  videomtx_get(uint8_t output);

/** Return a pointer to the full 16-byte routing table. */
const uint8_t *videomtx_get_all(void);

/** Register (or replace) the change notification callback. */
void     videomtx_set_notify(videomtx_notify_fn_t fn);
