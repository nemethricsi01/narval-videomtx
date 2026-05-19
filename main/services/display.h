#pragma once

#include "esp_err.h"
#include "lvgl.h"

/**
 * Initialise the ST7789 LCD, LVGL, draw buffers, tick timer, and LVGL task.
 * The display is rotated 90° (landscape 320×170) on return.
 *
 * @param[out] out_disp  Populated with the LVGL display handle for UI code.
 */
esp_err_t display_init(lv_display_t **out_disp);

/**
 * Acquire the LVGL API lock before creating or modifying widgets.
 * Always pair with display_unlock().
 */
void display_lock(void);

/** Release the LVGL API lock. */
void display_unlock(void);

/**
 * Set backlight brightness via LEDC PWM.
 * @param percent  0–100; applied on a quadratic (gamma-2) curve.
 */
void display_set_brightness(uint8_t percent);
