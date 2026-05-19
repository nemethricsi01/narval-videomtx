#pragma once

#include "esp_err.h"

typedef enum {
    ENCODER_CW,         // clockwise turn
    ENCODER_CCW,        // counter-clockwise turn
    ENCODER_CLICK,      // single button press (full press-and-release)
    ENCODER_LONG,       // long press start
    ENCODER_PRESS_DOWN, // button physically pressed down
    ENCODER_PRESS_UP,   // button physically released
} encoder_event_t;

typedef void (*encoder_cb_t)(encoder_event_t event, void *user_data);

/**
 * Initialise the rotary encoder and its push-button.
 * The callback is invoked from a timer/ISR context — keep it short.
 *
 * @param cb        Called on every encoder or button event.
 * @param user_data Passed through to every cb invocation.
 */
esp_err_t encoder_init(encoder_cb_t cb, void *user_data);

/** Accumulated step count (positive = CW, negative = CCW). */
int encoder_get_count(void);
