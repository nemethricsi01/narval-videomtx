#include "services/matrix_config.h"

#define CELL_INDEX(r, c)  ((uint16_t)(r) * MATRIX_COLS + (c))

uint8_t matrix_get_cell(const uint8_t *data, uint8_t row, uint8_t col)
{
    uint16_t cell_idx       = CELL_INDEX(row, col);
    uint8_t  byte_idx       = (uint8_t)(cell_idx / 2);
    uint8_t  is_upper_nibble = (uint8_t)(cell_idx & 1);

    uint8_t byte_val = data[byte_idx];
    return is_upper_nibble ? (uint8_t)((byte_val >> 4) & 0x0F)
                           : (uint8_t)(byte_val & 0x0F);
}

void matrix_set_cell(uint8_t *data, uint8_t row, uint8_t col, uint8_t state)
{
    uint16_t cell_idx       = CELL_INDEX(row, col);
    uint8_t  byte_idx       = (uint8_t)(cell_idx / 2);
    uint8_t  is_upper_nibble = (uint8_t)(cell_idx & 1);
    uint8_t  s              = state & 0x0F;

    if (is_upper_nibble)
        data[byte_idx] = (uint8_t)((data[byte_idx] & 0x0F) | (s << 4));
    else
        data[byte_idx] = (uint8_t)((data[byte_idx] & 0xF0) | s);
}
