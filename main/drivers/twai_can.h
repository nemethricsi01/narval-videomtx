#pragma once

#include "esp_err.h"
#include "esp_twai.h"

/**
 * Allocate and enable a TWAI node in listen-only mode (~9.8 kbps).
 *
 * @param[out] out_node  Populated with the node handle for receive calls.
 */
esp_err_t twai_can_init(twai_node_handle_t *out_node);
