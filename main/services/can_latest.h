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

/**
 * Re-derive all column state from the currently committed prog data and push
 * fresh LED states to the bus — equivalent to what boot does. Call this after
 * a live matrix/column-properties upload (see prog_set_commit_notify()) so
 * the new config takes effect without a reboot.
 */
void can_latest_reconfigure(void);

/**
 * Send the current state of every configured column to its device addresses.
 * slow=true adds a 15 ms inter-frame delay — use at startup to avoid exhausting
 * the TX pool.  Pass slow=false for runtime refresh requests.
 */
void can_latest_broadcast(bool slow);

/**
 * Print all configured column state (mode, base_addr, addrs, states) via ESP_LOGI.
 * Call after can_latest_configure() for startup diagnostics.
 */
void can_latest_dump(void);

/**
 * Sync one column's LED state to the bus after its route changed from
 * somewhere other than a real button press (e.g. the on-device UI matrix
 * editor). Call this right after videomtx_set_silent() for that output.
 *
 * If the new route isn't one of this column's configured states (the UI
 * allows picking any of the 16 inputs; a column only knows the ones its
 * matrix config gave it), this logs a warning and does not send anything —
 * there's no sane LED state to represent that route on the bus.
 */
void can_latest_notify_route(uint8_t output);

