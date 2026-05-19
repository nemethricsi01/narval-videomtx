#include "services/can.h"
#include "services/can_monitor.h"
#include "services/can_latest.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "can";

#define QUEUE_DEPTH   32
#define TASK_STACK    (4 * 1024)
#define TASK_PRIORITY  3
#define TX_POOL_SIZE  10   // must match twai_can tx_queue_depth

static QueueHandle_t      s_queue;
static QueueHandle_t      s_rx_observer;
static twai_node_handle_t s_node;

// TX pool — twai_node_transmit stores a pointer to twai_frame_t and its buffer;
// both must remain valid until the on_tx_done ISR fires for that frame.
// s_tx_busy is a bitmask: bit N is set while slot N is owned by the TWAI driver.
static twai_frame_t  s_tx_pool[TX_POOL_SIZE];
static uint8_t       s_tx_data[TX_POOL_SIZE][CAN_DATA_MAX_LEN];
static portMUX_TYPE  s_tx_mux  = portMUX_INITIALIZER_UNLOCKED;
static uint32_t      s_tx_busy = 0;   // bit N set = slot N in-flight

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

    const twai_event_callbacks_t cbs = {
        .on_rx_done = on_rx_done,
        .on_tx_done = on_tx_done,
    };
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(node, &cbs, NULL));
    ESP_ERROR_CHECK(twai_node_enable(node));

    xTaskCreate(can_task, "can", TASK_STACK, NULL, TASK_PRIORITY, NULL);

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

esp_err_t can_service_send(const can_frame_t *frame, int timeout_ms)
{
    // Find a free pool slot atomically
    portENTER_CRITICAL(&s_tx_mux);
    int idx = -1;
    for (int i = 0; i < TX_POOL_SIZE; i++) {
        if (!(s_tx_busy & (1u << i))) {
            idx = i;
            s_tx_busy |= (1u << i);   // claim before releasing the lock
            break;
        }
    }
    portEXIT_CRITICAL(&s_tx_mux);

    if (idx < 0) {
        return ESP_ERR_NO_MEM;   // all slots still held by TWAI driver
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
