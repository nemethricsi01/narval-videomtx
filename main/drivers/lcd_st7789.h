#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

/**
 * Initialise the ST7789 panel over SPI.
 * Turns backlight on at the end of init.
 *
 * @param[out] out_panel  Populated with the panel handle for draw calls.
 * @param[out] out_io     Populated with the panel IO handle (needed by display service).
 */
esp_err_t lcd_st7789_init(esp_lcd_panel_handle_t *out_panel,
                           esp_lcd_panel_io_handle_t *out_io);
