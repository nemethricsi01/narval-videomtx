#pragma once

#include <stdint.h>

#define PROG_MATRIX_LEN    128   /* 256 cells packed as 4-bit nibbles */
#define PROG_COL_PROPS_LEN  96   /* 16 columns × 6 bytes              */
#define PROG_PAYLOAD_LEN   224   /* MATRIX_LEN + COL_PROPS_LEN        */

/**
 * Initialise the programming service: zero active data, then load from NVS.
 * Call before usb_bridge_init().
 */
void prog_init(void);

/**
 * Handle one programming frame.  Called by the USB dispatcher after it
 * consumes the 0xBB start byte.  Reads the rest of the frame from the CDC
 * stream via cdc_read_byte(), acts on it, and sends the response via
 * cdc_write().
 *
 * Commands:
 *   0x01 WRITE  — stage 224-byte payload, echo frame back
 *   0x02 COMMIT — activate staging → flash, respond
 *   0x03 ABORT  — discard staging, respond
 *   0x04 READ   — send active data back
 */
void prog_poll(void);

/**
 * Return a pointer to the active (committed) matrix data (128 bytes).
 * Valid indefinitely; updated only on COMMIT.
 */
const uint8_t *prog_get_matrix(void);

/**
 * Return a pointer to the active column properties (96 bytes).
 * Valid indefinitely; updated only on COMMIT.
 */
const uint8_t *prog_get_col_props(void);

/**
 * Register a callback that fires right after a successful COMMIT (new
 * matrix + column properties saved to NVS and made active). Lets other
 * subsystems — e.g. can_latest — pick up the new config live, without a
 * reboot. Call before usb_bridge_init() so it's registered before a COMMIT
 * can actually land.
 */
void prog_set_commit_notify(void (*fn)(void));
