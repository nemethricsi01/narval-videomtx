#pragma once
#include "esp_err.h"
#include <stdint.h>

esp_err_t ws2812_init(int gpio_num);
esp_err_t ws2812_set(uint8_t r, uint8_t g, uint8_t b);
esp_err_t ws2812_off(void);
void      ws2812_deinit(void);
