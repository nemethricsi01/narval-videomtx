#include "services/column_properties.h"

uint8_t column_props_get(const uint8_t *data, uint8_t col, uint8_t prop_idx)
{
    if (col >= NUM_COLUMNS || prop_idx >= NUM_PROPERTIES) return 0;
    return data[col * NUM_PROPERTIES + prop_idx];
}

void column_props_set(uint8_t *data, uint8_t col, uint8_t prop_idx, uint8_t value)
{
    if (col >= NUM_COLUMNS || prop_idx >= NUM_PROPERTIES) return;
    data[col * NUM_PROPERTIES + prop_idx] = value;
}
