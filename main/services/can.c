#include "services/can.h"
#include "services/can_monitor.h"
#include "services/can_latest.h"
#include "driver/gpio.h"
#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "can";

#define QUEUE_DEPTH   32
#define TASK_STACK    (4 * 1024)
#define TASK_PRIORITY  3
#define TX_POOL_SIZE  10   // must match twai_can tx_queue_depth

#define LOG_TASK_STACK         2048
#define LED_PULSE_MS           50
#define STATE_EVT_QUEUE_DEPTH  8

// How often can_state_log_task re-checks node status even with no event
// pending — the safety net that catches a state-change notification the ISR
// couldn't queue (see on_state_change).
#define BUS_STATE_POLL_MS  2000

// A pool slot held longer than this is not "just slow" (a frame at ~9765 bps
// takes ~10 ms, even queued TX_POOL_SIZE-deep that's well under a second) —
// see reclaim_stale_tx_slots() for why one can get stuck.
#define TX_SLOT_STALE_US  (2 * 1000 * 1000)

// Once a bus-off recovery request is in flight, don't re-issue it just
// because the periodic poll still sees BUS_OFF — recovery takes a real
// (short) amount of bus-idle time to complete, and repeating the request
// while one is already progressing risks disrupting it. Only retry if it's
// been outstanding implausibly long (i.e. it was likely lost or failed).
#define RECOVERY_RETRY_TIMEOUT_US  (5 * 1000 * 1000)

static QueueHandle_t      s_queue;
static QueueHandle_t      s_rx_observer;
static twai_node_handle_t s_node;
static TaskHandle_t       s_err_log_task;
static QueueHandle_t      s_state_evt_q;
static volatile uint32_t  s_state_evt_dropped = 0;   // count of on_state_change events the queue couldn't hold

// TX pool — twai_node_transmit stores a pointer to twai_frame_t and its buffer;
// both must remain valid until the on_tx_done ISR fires for that frame.
// s_tx_busy is a bitmask: bit N is set while slot N is owned by the TWAI driver.
static twai_frame_t  s_tx_pool[TX_POOL_SIZE];
static uint8_t       s_tx_data[TX_POOL_SIZE][CAN_DATA_MAX_LEN];
static portMUX_TYPE  s_tx_mux  = portMUX_INITIALIZER_UNLOCKED;
static uint32_t      s_tx_busy = 0;   // bit N set = slot N in-flight
static int64_t       s_tx_claimed_at_us[TX_POOL_SIZE];  // set alongside the busy bit; see reclaim_stale_tx_slots()

// ---------------------------------------------------------------------------
// ISR callbacks
// ---------------------------------------------------------------------------

static bool IRAM_ATTR on_tx_done(twai_node_handle_t node,
                                  const twai_tx_done_event_data_t *edata,
                                  void *user_ctx)
{
    int idx = (int)(edata->done_tx_frame - s_tx_pool);
    if ((unsigned)idx < TX_POOL_SIZE) {
        portENTER_CRITICAL_ISR(&s_tx_mux);
        s_tx_busy &= ~(1u << idx);
        portEXIT_CRITICAL_ISR(&s_tx_mux);
    }
    return false;
}

static bool IRAM_ATTR on_rx_done(twai_node_handle_t node,
                                  const twai_rx_done_event_data_t *edata,
                                  void *user_ctx)
{
    can_frame_t frame = {0};
    twai_frame_t rx = {
        .buffer     = frame.data,
        .buffer_len = CAN_DATA_MAX_LEN,
    };

    if (twai_node_receive_from_isr(node, &rx) != ESP_OK) {
        return false;
    }

    frame.header = rx.header;

    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_queue, &frame, &woken);
    return woken == pdTRUE;
}

// Fires on every bit/stuff/form/ack/arbitration error the controller detects
// on the bus — including on frames this node only received, never sent.
// Just logs (via can_error_log_task) — no LED tied to this anymore; GPIO48
// is now the "frame is ours" indicator driven from can_latest_update().
static bool IRAM_ATTR on_error(twai_node_handle_t node,
                                const twai_error_event_data_t *edata,
                                void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xTaskNotifyFromISR(s_err_log_task, edata->err_flags.val, eSetValueWithOverwrite, &woken);
    return woken == pdTRUE;
}

// Fires whenever the controller's error state changes (ACTIVE -> WARNING ->
// PASSIVE -> BUS_OFF, and back down again after bus-off recovery). Queued
// (not overwrite-coalesced like on_error) so a transition is never dropped —
// there are only ever a handful of these, unlike bit-level error flags.
// If the queue is ever actually full, count the drop rather than lose it
// silently: can_state_log_task's periodic poll still notices a stuck
// BUS_OFF regardless, so a dropped notification here can't make bus-off
// permanent again — it only delays detection by up to one poll period.
static bool IRAM_ATTR on_state_change(twai_node_handle_t node,
                                       const twai_state_change_event_data_t *edata,
                                       void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s_state_evt_q, edata, &woken) != pdTRUE) {
        s_state_evt_dropped++;
    }
    return woken == pdTRUE;
}

// ---------------------------------------------------------------------------
// Error log task — logs bus error flags from task context (on_error fires
// from ISR context, where ESP_LOGx is not safe to call).
// ---------------------------------------------------------------------------

static void can_error_log_task(void *arg)
{
    while (1) {
        uint32_t flags = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &flags, portMAX_DELAY) == pdTRUE && flags) {
            twai_error_flags_t err = { .val = flags };
            ESP_LOGW(TAG, "bus error:%s%s%s%s%s",
                     err.arb_lost ? " arb_lost" : "",
                     err.bit_err  ? " bit_err"  : "",
                     err.form_err ? " form_err" : "",
                     err.stuff_err ? " stuff_err" : "",
                     err.ack_err  ? " ack_err"  : "");
        }
    }
}

// ---------------------------------------------------------------------------
// State-change log task — also the bus-off recovery driver. The TWAI driver
// never recovers from bus-off on its own; it requires an explicit
// twai_node_recover() call, after which the hardware needs 128 consecutive
// occurrences of 11 recessive bits on the wire before it goes back to ACTIVE
// (reported via another on_state_change). Without this task, bus-off is
// permanent until reboot — which was the original symptom this exists to fix.
// ---------------------------------------------------------------------------

static const char *error_state_name(twai_error_state_t s)
{
    switch (s) {
    case TWAI_ERROR_ACTIVE:  return "ACTIVE";
    case TWAI_ERROR_WARNING: return "WARNING";
    case TWAI_ERROR_PASSIVE: return "PASSIVE";
    case TWAI_ERROR_BUS_OFF: return "BUS_OFF";
    default:                 return "?";
    }
}

static bool    s_recovery_in_flight       = false;  // can_state_log_task-only, no lock needed
static int64_t s_recovery_requested_at_us = 0;

static void can_state_log_task(void *arg)
{
    twai_state_change_event_data_t evt;
    while (1) {
        // Timeout (rather than portMAX_DELAY) turns this into a periodic
        // safety-net poll as well as an event consumer — see on_state_change
        // and the module doc-comment above for why that matters.
        BaseType_t got = xQueueReceive(s_state_evt_q, &evt, pdMS_TO_TICKS(BUS_STATE_POLL_MS));

        if (s_state_evt_dropped) {
            ESP_LOGW(TAG, "%lu bus state-change notification(s) dropped (queue full)",
                     (unsigned long)s_state_evt_dropped);
            s_state_evt_dropped = 0;
        }

        twai_node_status_t status = {0};
        esp_err_t info_err = twai_node_get_info(s_node, &status, NULL);
        if (info_err != ESP_OK) {
            ESP_LOGE(TAG, "twai_node_get_info failed: %s", esp_err_to_name(info_err));
            continue;   // nothing safe to act on this cycle; try again next poll
        }

        if (got == pdTRUE) {
            ESP_LOGW(TAG, "bus state: %s -> %s  (TEC=%u REC=%u)",
                     error_state_name(evt.old_sta), error_state_name(evt.new_sta),
                     (unsigned)status.tx_error_count, (unsigned)status.rx_error_count);
        }

        if (status.state != TWAI_ERROR_BUS_OFF) {
            s_recovery_in_flight = false;   // clear so the next bus-off starts a fresh request
            continue;
        }

        int64_t now = esp_timer_get_time();
        bool retry_timed_out = s_recovery_in_flight &&
                                (now - s_recovery_requested_at_us) > RECOVERY_RETRY_TIMEOUT_US;

        if (!s_recovery_in_flight || retry_timed_out) {
            if (retry_timed_out) {
                ESP_LOGW(TAG, "bus-off still active %lld ms after last recovery request — retrying",
                         (long long)((now - s_recovery_requested_at_us) / 1000));
            } else {
                ESP_LOGW(TAG, "bus-off — requesting recovery");
            }
            esp_err_t err = twai_node_recover(s_node);
            if (err == ESP_OK) {
                s_recovery_in_flight       = true;
                s_recovery_requested_at_us = now;
            } else {
                ESP_LOGE(TAG, "twai_node_recover failed: %s", esp_err_to_name(err));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// "Frame is ours" LED — default on; can_led_pulse_ours() briefly turns it
// off. Called synchronously from can_latest_update() (can_task context), so
// a blocking delay here is fine.
// ---------------------------------------------------------------------------

static void can_led_init(void)
{
    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << BOARD_PIN_CAN_LED,
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));
    gpio_set_level(BOARD_PIN_CAN_LED, 1);   // default on
}

void can_led_pulse_ours(void)
{
    gpio_set_level(BOARD_PIN_CAN_LED, 0);
    vTaskDelay(pdMS_TO_TICKS(LED_PULSE_MS));
    gpio_set_level(BOARD_PIN_CAN_LED, 1);
}

// ---------------------------------------------------------------------------
// Processing task — consumes frames from the queue
// ---------------------------------------------------------------------------

static void can_task(void *arg)
{
    can_frame_t frame;
    while (1) {
        if (xQueueReceive(s_queue, &frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        uint16_t len = (frame.header.dlc < CAN_DATA_MAX_LEN) ? frame.header.dlc : CAN_DATA_MAX_LEN;
        ESP_LOGI(TAG, "[%s 0x%0*lx] len=%u  %02x %02x %02x %02x %02x %02x %02x %02x",
                 frame.header.ide ? "EXT" : "STD",
                 frame.header.ide ? 8 : 3,
                 (unsigned long)frame.header.id,
                 (unsigned)len,
                 frame.data[0], frame.data[1], frame.data[2], frame.data[3],
                 frame.data[4], frame.data[5], frame.data[6], frame.data[7]);
        can_mon_push(&frame, false, false);
        can_latest_update(&frame);
        if (s_rx_observer) {
            xQueueSend(s_rx_observer, &frame, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t can_service_init(twai_node_handle_t node)
{
    s_node  = node;
    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(can_frame_t));
    assert(s_queue);

    can_led_init();

    s_state_evt_q = xQueueCreate(STATE_EVT_QUEUE_DEPTH, sizeof(twai_state_change_event_data_t));
    assert(s_state_evt_q);

    // Both log/recovery tasks must exist before callbacks are registered/node
    // is enabled — on_error and on_state_change notify them from ISR context
    // as soon as an error or state transition occurs.
    BaseType_t ok = xTaskCreate(can_error_log_task, "can_err_log", LOG_TASK_STACK, NULL, TASK_PRIORITY, &s_err_log_task);
    assert(ok == pdPASS);
    ok = xTaskCreate(can_state_log_task, "can_state_log", LOG_TASK_STACK, NULL, TASK_PRIORITY, NULL);
    assert(ok == pdPASS);

    const twai_event_callbacks_t cbs = {
        .on_rx_done      = on_rx_done,
        .on_tx_done      = on_tx_done,
        .on_error        = on_error,
        .on_state_change = on_state_change,
    };
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(node, &cbs, NULL));
    ESP_ERROR_CHECK(twai_node_enable(node));

    ok = xTaskCreate(can_task, "can", TASK_STACK, NULL, TASK_PRIORITY, NULL);
    assert(ok == pdPASS);

    ESP_LOGI(TAG, "ready (queue depth %d)", QUEUE_DEPTH);
    return ESP_OK;
}

QueueHandle_t can_service_get_queue(void)
{
    return s_queue;
}

void can_service_add_rx_observer(QueueHandle_t q)
{
    s_rx_observer = q;
}

// Claims one free TX pool slot atomically and stamps it with the current
// time (used by reclaim_stale_tx_slots()). The stamp is set inside the same
// critical section as the busy bit so a slot can never be observed as "busy"
// with a stale leftover timestamp from its previous use.
static int try_claim_tx_slot(void)
{
    int idx = -1;
    portENTER_CRITICAL(&s_tx_mux);
    for (int i = 0; i < TX_POOL_SIZE; i++) {
        if (!(s_tx_busy & (1u << i))) {
            idx = i;
            s_tx_busy |= (1u << i);
            s_tx_claimed_at_us[i] = esp_timer_get_time();
            break;
        }
    }
    portEXIT_CRITICAL(&s_tx_mux);
    return idx;
}

// Reclaims TX pool slots held implausibly long. Needed because of one
// specific driver behavior: if bus-off begins while a frame is actively on
// the wire, that frame is silently dropped by the hardware WITHOUT ever
// firing on_tx_done (only a completed transmission attempt does), so its
// pool slot's bit in s_tx_busy is never cleared by the normal path. Since
// every frame on this bus shares CAN ID 0x000 (no arbitration between
// nodes), bus-off is expected to recur over the unit's lifetime — left
// unreclaimed, each occurrence could permanently strand one of only
// TX_POOL_SIZE slots, eventually exhausting the pool even after the bus
// itself has long since recovered.
// Called opportunistically from can_service_send() only once the pool looks
// full, so it costs nothing on the normal/healthy path.
static void reclaim_stale_tx_slots(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_tx_mux);
    uint32_t before = s_tx_busy;
    for (int i = 0; i < TX_POOL_SIZE; i++) {
        if ((s_tx_busy & (1u << i)) && (now - s_tx_claimed_at_us[i]) > TX_SLOT_STALE_US) {
            s_tx_busy &= ~(1u << i);
        }
    }
    uint32_t reclaimed = before & ~s_tx_busy;
    portEXIT_CRITICAL(&s_tx_mux);

    if (reclaimed) {
        ESP_LOGW(TAG, "reclaimed %d stale TX pool slot(s) (mask 0x%03lx) — "
                 "likely dropped mid-transmission by a bus-off",
                 __builtin_popcount(reclaimed), (unsigned long)reclaimed);
    }
}

esp_err_t can_service_send(const can_frame_t *frame, int timeout_ms)
{
    int idx = try_claim_tx_slot();
    if (idx < 0) {
        // Pool looked full — reclaim anything the driver silently dropped
        // (see reclaim_stale_tx_slots()) and retry once before giving up.
        reclaim_stale_tx_slots();
        idx = try_claim_tx_slot();
        if (idx < 0) {
            ESP_LOGW(TAG, "TX pool exhausted (%d slots all busy)", TX_POOL_SIZE);
            return ESP_ERR_NO_MEM;   // all slots still genuinely held by TWAI driver
        }
    }

    uint8_t dlc = (frame->header.dlc < CAN_DATA_MAX_LEN) ? frame->header.dlc : CAN_DATA_MAX_LEN;
    memcpy(s_tx_data[idx], frame->data, dlc);

    s_tx_pool[idx].header     = frame->header;
    s_tx_pool[idx].buffer     = s_tx_data[idx];
    s_tx_pool[idx].buffer_len = dlc;

    esp_err_t ret = twai_node_transmit(s_node, &s_tx_pool[idx], timeout_ms);
    if (ret != ESP_OK) {
        // Enqueue failed — release the slot immediately since TWAI never took it
        portENTER_CRITICAL(&s_tx_mux);
        s_tx_busy &= ~(1u << idx);
        portEXIT_CRITICAL(&s_tx_mux);
    }
    return ret;
}
