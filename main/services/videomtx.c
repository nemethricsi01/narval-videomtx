#include "services/videomtx.h"
#include "drivers/videomtx_ic.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "videomtx";

#define NVS_NS   "videomtx"
#define NVS_KEY  "route"

// Debounce for NVS writes: the physical crosspoint IC is updated immediately
// on every change (see do_set()) — only flash persistence is delayed. A
// burst of edits (e.g. reconfiguring several outputs in a row) collapses
// into one write instead of one per change, since NVS writes cost real flash
// wear over the unit's lifetime and the video routing itself doesn't need
// the save to be instantaneous.
#define SAVE_DEBOUNCE_MS   (30 * 1000)        // quiet period required after the last change before writing
#define SAVE_MAX_DELAY_MS  (10 * 60 * 1000)   // upper bound: force a save at least this often even under continuous edits

// A line transient on the crosspoint IC's wiring can wipe its latched
// switch state without touching s_route[] (firmware's own copy is
// unaffected) — confirmed on hardware: sending the routing table again is
// enough to recover it, no reset pulse needed. Since we can't rely on a
// user or CAN command ever actually changing a route again, periodically
// re-push the current table unconditionally so a transient self-heals
// within one interval instead of persisting indefinitely.
// Confirmed on this IC: re-latching identical data has no visible effect on
// the outputs, so this can run this often for cheap.
// TODO: the IC supports SPI readback of its latched state (MISO is wired,
// board.h's BOARD_PIN_MTX_MISO) but videomtx_ic_write() only ever writes —
// a future version could read back and compare, only rewriting (pulsing
// UPDATE) on an actual mismatch, and log/count how often transients really
// happen. Deferred for now in favor of this simpler blind refresh.
#define IC_REFRESH_INTERVAL_MS  (2 * 1000)

static uint8_t              s_route[VIDEOMTX_SIZE];
static videomtx_notify_fn_t s_notify_fn;
static SemaphoreHandle_t    s_save_sem;
static SemaphoreHandle_t    s_route_mutex;   // protects s_route — see do_set() and nvs_save_task()

/* Runs on a dedicated low-priority task so flash writes never block the LVGL
 * task. Debounces: after the first change, keep waiting as long as another
 * change arrives within SAVE_DEBOUNCE_MS (each one just re-arms the same
 * binary semaphore, so bursts collapse for free), but never wait past
 * SAVE_MAX_DELAY_MS total since the first change in the burst — a "force
 * save" so a very long editing session still gets persisted periodically. */
static void nvs_save_task(void *arg)
{
    while (1) {
        xSemaphoreTake(s_save_sem, portMAX_DELAY);   // wait for the first change after being idle

        int64_t first_change_us = esp_timer_get_time();
        while (1) {
            int64_t elapsed_ms   = (esp_timer_get_time() - first_change_us) / 1000;
            int64_t remaining_ms = SAVE_MAX_DELAY_MS - elapsed_ms;
            if (remaining_ms <= 0)
                break;   // hit the cap — force a save now even if changes are still coming in
            TickType_t wait = pdMS_TO_TICKS(
                (remaining_ms < SAVE_DEBOUNCE_MS) ? (uint32_t)remaining_ms : SAVE_DEBOUNCE_MS);
            if (xSemaphoreTake(s_save_sem, wait) != pdTRUE)
                break;   // quiet period elapsed with no further changes — save now
        }

        // Snapshot under the same mutex do_set() holds while it writes a
        // change — even the force-save path (which can fire while changes
        // are still actively arriving) can only ever see a fully-applied
        // s_route, never one do_set() is mid-write on. The lock is only
        // held for a 16-byte memcpy; the (slower) actual flash write below
        // happens after releasing it, so it never delays a pending do_set().
        uint8_t snapshot[VIDEOMTX_SIZE];
        xSemaphoreTake(s_route_mutex, portMAX_DELAY);
        memcpy(snapshot, s_route, VIDEOMTX_SIZE);
        xSemaphoreGive(s_route_mutex);

        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
            ESP_LOGE(TAG, "nvs_open failed");
            continue;
        }
        nvs_set_blob(h, NVS_KEY, snapshot, VIDEOMTX_SIZE);
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

// See IC_REFRESH_INTERVAL_MS above for why this exists. Independent of
// nvs_save_task — this is about recovering the IC's volatile hardware
// state, not about flash wear, so it runs on its own unconditional timer
// rather than being debounced by activity.
static void ic_refresh_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(IC_REFRESH_INTERVAL_MS));
        xSemaphoreTake(s_route_mutex, portMAX_DELAY);
        videomtx_ic_write(s_route);
        xSemaphoreGive(s_route_mutex);
    }
}

void videomtx_init(void)
{
    for (int i = 0; i < VIDEOMTX_SIZE; i++)
        s_route[i] = (uint8_t)i;
    s_notify_fn = NULL;
    nvs_load();

    s_save_sem    = xSemaphoreCreateBinary();
    s_route_mutex = xSemaphoreCreateMutex();
    xTaskCreate(nvs_save_task, "vmtx_nvs", 2048, NULL, 3, NULL);

    ESP_ERROR_CHECK(videomtx_ic_init());
    videomtx_ic_write(s_route);  // push the routing table loaded from NVS

    xTaskCreate(ic_refresh_task, "vmtx_ic_refresh", 2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "ready");
}

static void do_set(uint8_t output, uint8_t input, bool notify)
{
    if (output >= VIDEOMTX_SIZE || input >= VIDEOMTX_SIZE) return;
    // Held across the array write and the physical IC push so a concurrent
    // save (debounced or force-capped, see nvs_save_task()) can never
    // observe s_route while a change is still being applied.
    xSemaphoreTake(s_route_mutex, portMAX_DELAY);
    s_route[output] = input;
    videomtx_ic_write(s_route);
    xSemaphoreGive(s_route_mutex);
    ESP_LOGI(TAG, "out%02d <- in%02d", output + 1, input + 1);
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
