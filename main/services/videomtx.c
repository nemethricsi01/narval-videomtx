#include "services/videomtx.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "videomtx";

#define NVS_NS   "videomtx"
#define NVS_KEY  "route"

static uint8_t              s_route[VIDEOMTX_SIZE];
static videomtx_notify_fn_t s_notify_fn;
static SemaphoreHandle_t    s_save_sem;

/* Runs on a dedicated low-priority task so flash writes never block the LVGL
 * task.  The binary semaphore collapses multiple rapid changes into one save. */
static void nvs_save_task(void *arg)
{
    while (1) {
        xSemaphoreTake(s_save_sem, portMAX_DELAY);
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
            ESP_LOGE(TAG, "nvs_open failed");
            continue;
        }
        nvs_set_blob(h, NVS_KEY, s_route, VIDEOMTX_SIZE);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "saved to NVS");
    }
}

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = VIDEOMTX_SIZE;
    nvs_get_blob(h, NVS_KEY, s_route, &len);
    nvs_close(h);
}

static void spi_dump(void)
{
    /* Mock SPI: log full routing table as a compact one-liner.
     * Replace with real SPI transfer once hardware is available. */
    char buf[128];
    int  off = 0;
    off += snprintf(buf + off, sizeof(buf) - off, "SPI:");
    for (int i = 0; i < VIDEOMTX_SIZE; i++)
        off += snprintf(buf + off, sizeof(buf) - off, " %02d>%02d", i + 1, s_route[i] + 1);
    ESP_LOGI(TAG, "%s", buf);
}

void videomtx_init(void)
{
    for (int i = 0; i < VIDEOMTX_SIZE; i++)
        s_route[i] = (uint8_t)i;
    s_notify_fn = NULL;
    nvs_load();

    s_save_sem = xSemaphoreCreateBinary();
    xTaskCreate(nvs_save_task, "vmtx_nvs", 2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "ready");
}

static void do_set(uint8_t output, uint8_t input, bool notify)
{
    if (output >= VIDEOMTX_SIZE || input >= VIDEOMTX_SIZE) return;
    s_route[output] = input;
    ESP_LOGI(TAG, "out%02d <- in%02d", output + 1, input + 1);
    spi_dump();
    xSemaphoreGive(s_save_sem);
    if (notify && s_notify_fn) s_notify_fn(output, input);
}

void videomtx_set(uint8_t output, uint8_t input)        { do_set(output, input, true);  }
void videomtx_set_silent(uint8_t output, uint8_t input) { do_set(output, input, false); }

uint8_t videomtx_get(uint8_t output)
{
    if (output >= VIDEOMTX_SIZE) return 0;
    return s_route[output];
}

const uint8_t *videomtx_get_all(void)
{
    return s_route;
}

void videomtx_set_notify(videomtx_notify_fn_t fn)
{
    s_notify_fn = fn;
}
