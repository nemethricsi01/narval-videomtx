#include "drivers/encoder.h"
#include "board.h"
#include "iot_knob.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "esp_log.h"

static const char *TAG = "encoder";

static encoder_cb_t   s_cb;
static void          *s_user_data;
static knob_handle_t  s_knob;

// ---------------------------------------------------------------------------
// Knob callbacks
// ---------------------------------------------------------------------------

static void on_knob_right(void *arg, void *data)
{
    if (s_cb) s_cb(ENCODER_CW, s_user_data);
}

static void on_knob_left(void *arg, void *data)
{
    if (s_cb) s_cb(ENCODER_CCW, s_user_data);
}

// ---------------------------------------------------------------------------
// Button callbacks
// ---------------------------------------------------------------------------

static void on_btn_click(void *arg, void *data)
{
    if (s_cb) s_cb(ENCODER_CLICK, s_user_data);
}

static void on_btn_long(void *arg, void *data)
{
    if (s_cb) s_cb(ENCODER_LONG, s_user_data);
}

static void on_btn_down(void *arg, void *data)
{
    if (s_cb) s_cb(ENCODER_PRESS_DOWN, s_user_data);
}

static void on_btn_up(void *arg, void *data)
{
    if (s_cb) s_cb(ENCODER_PRESS_UP, s_user_data);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t encoder_init(encoder_cb_t cb, void *user_data)
{
    s_cb        = cb;
    s_user_data = user_data;

    // Rotary encoder
    knob_config_t knob_cfg = {
        .default_direction = 0,   // 0: CW = positive count
        .gpio_encoder_a    = BOARD_PIN_ENC_A,
        .gpio_encoder_b    = BOARD_PIN_ENC_B,
    };
    s_knob = iot_knob_create(&knob_cfg);
    if (!s_knob) {
        ESP_LOGE(TAG, "iot_knob_create failed");
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(iot_knob_register_cb(s_knob, KNOB_RIGHT, on_knob_right, NULL));
    ESP_ERROR_CHECK(iot_knob_register_cb(s_knob, KNOB_LEFT,  on_knob_left,  NULL));

    // Push-button
    button_config_t btn_cfg = {
        .long_press_time  = 0,  // use component default
        .short_press_time = 0,
    };
    button_gpio_config_t gpio_cfg = {
        .gpio_num     = BOARD_PIN_ENC_BTN,
        .active_level = BOARD_ENC_BTN_ACTIVE_LVL,
    };
    button_handle_t btn = NULL;
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn));
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_PRESS_DOWN,      NULL, on_btn_down,  NULL));
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_PRESS_UP,        NULL, on_btn_up,    NULL));
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_SINGLE_CLICK,    NULL, on_btn_click, NULL));
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, NULL, on_btn_long, NULL));

    ESP_LOGI(TAG, "ready (A=%d B=%d BTN=%d)",
             BOARD_PIN_ENC_A, BOARD_PIN_ENC_B, BOARD_PIN_ENC_BTN);
    return ESP_OK;
}

int encoder_get_count(void)
{
    return s_knob ? iot_knob_get_count_value(s_knob) : 0;
}
