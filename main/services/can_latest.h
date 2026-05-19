#pragma once

#include "can.h"
#include <stdbool.h>

/**
 * Create the internal mutex.
 * Must be called before any task that calls can_latest_update().
 */
void can_latest_init(void);

/**
 * Overwrite the stored frame with the most recently received raw frame.
 * Safe to call from any FreeRTOS task.
 */
void can_latest_update(const can_frame_t *frame);

/**
 * Copy the most recent frame into *out.
 * Returns true if at least one frame has been received since boot.
 * Thread-safe; never blocks longer than one mutex acquisition.
 */
bool can_latest_get(can_frame_t *out);

/**
 * Load initial state from the committed prog settings (matrix + column props).
 * Must be called after prog_init() — reads NVS-loaded data to build ledOrder
 * and other per-column configuration used at runtime.
 */
void can_latest_configure(void);
