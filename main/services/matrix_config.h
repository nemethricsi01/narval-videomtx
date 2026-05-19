#pragma once

#include <stdint.h>

#define MATRIX_ROWS  16
#define MATRIX_COLS  16   /* 16×16 = 256 cells, packed as 4-bit nibbles = 128 bytes */

/**
 * 4-bit cell packing matching Python matrix_storage.py:
 *   little-endian nibbles — lower nibble = even cell index, upper = odd
 *   byte index = (row * MATRIX_COLS + col) / 2
 *
 * Use prog_get_matrix() as the data pointer for the committed USB settings.
 */

 /**
 * @brief Read the state of one cell from the packed matrix buffer.
 *
 * @param data  Pointer to 128-byte packed matrix buffer.
 * @param row   Row index (input channel), 0..15.
 * @param col   Column index (output channel), 0..15.
 * @return      Cell state: CELL_GREY(0) .. CELL_YELLOW(4).
 */
uint8_t matrix_get_cell(const uint8_t *data, uint8_t row, uint8_t col);
/**
 * @brief Write the state of one cell into the packed matrix buffer.
 *
 * @param data   Pointer to 128-byte packed matrix buffer.
 * @param row    Row index (input channel), 0..15.
 * @param col    Column index (output channel), 0..15.
 * @param state  Cell state: CELL_GREY(0) .. CELL_YELLOW(4).
 *               Values above 4 are masked to the lower nibble.
 */
void    matrix_set_cell(uint8_t *data, uint8_t row, uint8_t col, uint8_t state);
