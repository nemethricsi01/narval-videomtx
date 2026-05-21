

/**
 * @file column_properties.h
 * @brief Per-column configuration — LED group addressing, button mode, device addresses.
 *
 * ============================================================
 * OVERVIEW
 * ============================================================
 *
 * Each of the 16 output columns has 6 configuration bytes that control:
 *   - Which interaction mode the column uses (button mode)
 *   - Where its LED group(s) start in the LED address space
 *   - Which bus devices receive state-update messages for this column
 *
 * ============================================================
 * PROPERTY REFERENCE  (prop_idx 0..5)
 * ============================================================
 *
 * prop[0] — Button Mode
 * ----------------------
 *   Controls how the column behaves when triggered.
 *
 *   Value | Name                 | LED groups | Description
 *   ------+----------------------+------------+-------------------------------
 *     0   | Normal (4-step)      |     1      | One LED group cycles through
 *         |                      |            | up to 4 states: GREEN → RED →
 *         |                      |            | YELLOW → BLACK.  Each state
 *         |                      |            | routes to the matching row.
 *   ------+----------------------+------------+-------------------------------
 *     1   | 6-step               |     1      | One LED group, 6 ordered steps
 *         |                      |            | (2×GREEN, 2×RED, 2×YELLOW).
 *         |                      |            | Two buttons step forward/back.
 *   ------+----------------------+------------+-------------------------------
 *     2   | Radiobutton Green    |   1 per    | One LED group per active row.
 *         |                      | active row | Exactly one green LED on at a
 *         |                      |            | time (radiobutton behavior).
 *         |                      |            | Addresses are sequential from
 *         |                      |            | prop[1].
 *   ------+----------------------+------------+-------------------------------
 *     3   | Radiobutton Red      |   1 per    | Same as mode 2 but drives the
 *         |                      | active row | red LED.  Assign the same
 *         |                      |            | prop[1] value as a mode-2
 *         |                      |            | column to share LED groups —
 *         |                      |            | both green and red banks then
 *         |                      |            | control the same physical LEDs.
 *
 * prop[1] — Starting LED Layer (LED group base address)
 * ------------------------------------------------------
 *   Range: 0..255
 *
 *   The address of the first LED group assigned to this column.
 *
 *   In normal / 6-step mode: there is exactly one LED group; its address
 *   is prop[1].
 *
 *   In radiobutton mode: LED groups are allocated sequentially.
 *     - Active row 0 → LED group at address prop[1]
 *     - Active row 1 → LED group at address prop[1] + 1
 *     - Active row N → LED group at address prop[1] + N
 *
 *   Two columns that share the same prop[1] value share the same physical
 *   LED groups.  This is how a Radiobutton Green column (mode 2) and a
 *   Radiobutton Red column (mode 3) are linked: they are configured with
 *   the same starting address so their LED groups are identical hardware.
 *
 * prop[2..5] — Device Addresses 1..4
 * ------------------------------------
 *   Range: 0..254 (0xFF = slot unused)
 *
 *   Up to 4 addresses of bus devices that must receive a state-update
 *   message whenever this column changes its active routing.
 *
 *   Purpose: keep all relevant devices in sync across the bus.
 *   When a trigger arrives from any device, the controller:
 *     1. Updates the active routing for this column.
 *     2. Sends the new LED state to every address listed in prop[2..5].
 *
 *   The triggering device's own address should be included in the list
 *   so it also receives the confirmation update.
 *
 *   Unused slots must be set to 0xFF.
 *
 *   Example — column 3 responds to device 10 and also keeps device 14
 *   and device 22 in sync:
 *     prop[2] = 10
 *     prop[3] = 14
 *     prop[4] = 22
 *     prop[5] = 0xFF   (unused)
 *
 * ============================================================
 * STORAGE FORMAT
 * ============================================================
 *
 * 16 columns × 6 bytes = 96 bytes, column-major layout:
 *
 *   col 0: bytes  0.. 5   (prop[0]..prop[5])
 *   col 1: bytes  6..11
 *   ...
 *   col 15: bytes 90..95
 *
 *   Byte index = col * NUM_PROPERTIES + prop_idx
 *
 * ============================================================
 * USAGE EXAMPLE
 * ============================================================
 *
 *   uint8_t col_props[COLUMN_PROPS_BYTES];
 *
 *   // Read the mode of column 4
 *   uint8_t mode = column_props_get(col_props, 4, 0);
 *
 *   // Read the LED group base address of column 4
 *   uint8_t base_addr = column_props_get(col_props, 4, 1);
 *
 *   // Iterate over the device addresses for column 4
 *   for (uint8_t i = 2; i < NUM_PROPERTIES; i++) {
 *       uint8_t dev_addr = column_props_get(col_props, 4, i);
 *       if (dev_addr == 0xFF) continue;  // unused slot
 *       send_update(dev_addr, new_state);
 *   }
 *
 *   // Set column 7 to radiobutton-green mode, base address 20
 *   column_props_set(col_props, 7, 0, 2);   // mode = Radiobutton Green
 *   column_props_set(col_props, 7, 1, 20);  // LED groups start at address 20
 */
#pragma once

#include <stdint.h>

#define NUM_COLUMNS        16
#define NUM_PROPERTIES     6
#define COLUMN_PROPS_BYTES (NUM_COLUMNS * NUM_PROPERTIES)  /* 96 */

/* prop[0] — Button Mode values */
#define MODE_NORMAL            0  /* 1 button, up to 4 steps (GREEN/RED/YELLOW/BLACK) */
#define MODE_6STEP             1  /* 2 buttons, 6 steps (2x GREEN, 2x RED, 2x YELLOW) */
#define MODE_RADIO_GREEN       2  /* radiobutton bank, green LED per group */
#define MODE_RADIO_RED         3  /* radiobutton bank, red LED per group */
#define MODE_NORMAL_SECONDARY  4  /* same as MODE_NORMAL; LED group base address is +1 */
#define MODE_6STEP_SECONDARY   5  /* same as MODE_6STEP; LED group base address is +2 */

/* prop index names for readability */
#define PROP_MODE          0
#define PROP_LED_BASE_ADDR 1
#define PROP_DEV_ADDR_1    2
#define PROP_DEV_ADDR_2    3
#define PROP_DEV_ADDR_3    4
#define PROP_DEV_ADDR_4    5

/**
 * Column-major layout matching Python column_properties.py:
 *   byte index = col * NUM_PROPERTIES + prop_idx
 *
 * Use prog_get_col_props() as the data pointer for the committed USB settings.
 */

 /**
 * @brief Read one property byte for a column.
 *
 * @param data      Pointer to 96-byte column properties buffer.
 * @param col       Column (output channel) index, 0..15.
 * @param prop_idx  Property index, 0..5 (use PROP_* constants).
 * @return          Property value 0..255, or 0 if indices are out of range.
 */
uint8_t column_props_get(const uint8_t *data, uint8_t col, uint8_t prop_idx);
/**
 * @brief Write one property byte for a column.
 *
 * @param data      Pointer to 96-byte column properties buffer.
 * @param col       Column (output channel) index, 0..15.
 * @param prop_idx  Property index, 0..5 (use PROP_* constants).
 * @param value     Value to write, 0..255.
 */
void    column_props_set(uint8_t *data, uint8_t col, uint8_t prop_idx, uint8_t value);
