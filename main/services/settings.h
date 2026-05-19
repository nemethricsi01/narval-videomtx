#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint8_t brightness_pct;   // 20–100
} settings_t;

#define SETTINGS_DEFAULT ((settings_t){ .brightness_pct = 100 })

esp_err_t settings_load(settings_t *s);
esp_err_t settings_save(const settings_t *s);
