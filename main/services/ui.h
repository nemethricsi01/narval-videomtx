#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"
#include "services/settings.h"

/**
 * Create all screens, set up groups, and load the main screen.
 * Must be called inside display_lock() / display_unlock().
 * Populates all widgets from the provided settings (loaded from NVS by the caller).
 */
esp_err_t ui_init(lv_indev_t *encoder_indev, const settings_t *s);

/** Open the menu and reset focus to the first item (entry from long-press). */
void ui_open_menu(void);

/** Return to the menu, preserving the current focus (exit from a submenu). */
void ui_show_menu(void);

/** Switch back to the main screen. */
void ui_show_main(void);

/**
 * Deliver one raw encoder event to the UI.  Must be called via lv_async_call
 * so it executes inside the LVGL task.
 * Usage: lv_async_call(ui_encoder_event, (void *)(uintptr_t)event);
 */
void ui_encoder_event(void *event_as_ptr);
