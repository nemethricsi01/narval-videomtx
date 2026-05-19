#include "services/settings.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "settings";
#define NS "settings"

esp_err_t settings_load(settings_t *s)
{
    *s = SETTINGS_DEFAULT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no saved settings, using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t v;
    if (nvs_get_u8(h, "brightness", &v) == ESP_OK)
        s->brightness_pct = (v >= 20 && v <= 100) ? v : 100;

    nvs_close(h);
    ESP_LOGI(TAG, "loaded");
    return ESP_OK;
}

esp_err_t settings_save(const settings_t *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_u8(h, "brightness", (uint8_t)s->brightness_pct);

    err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) ESP_LOGI(TAG, "saved");
    else               ESP_LOGW(TAG, "commit: %s", esp_err_to_name(err));
    return err;
}
