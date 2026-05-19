#pragma once

#include "esp_err.h"
#include "esp_twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define CAN_DATA_MAX_LEN  8   // classic CAN frame maximum

typedef struct {
    twai_frame_header_t header;
    uint8_t             data[CAN_DATA_MAX_LEN];
} can_frame_t;

/**
 * Register RX callback on the node, create the frame queue, and start the
 * processing task.
 *
 * @param node  Handle returned by twai_can_init().
 */
esp_err_t can_service_init(twai_node_handle_t node);

/**
 * Queue handle for can_frame_t items.
 * The service task consumes from this queue.
 * Post to it with xQueueSend() to inject frames (e.g., for testing).
 */
QueueHandle_t can_service_get_queue(void);

/**
 * Transmit a frame on the CAN bus.
 *
 * @param frame       Frame to send.
 * @param timeout_ms  Max wait if the TX queue is full; 0 = non-blocking.
 */
esp_err_t can_service_send(const can_frame_t *frame, int timeout_ms);

/**
 * Register an additional queue that receives a copy of every RX frame.
 * Must be called after can_service_init().  Only one observer is supported.
 *
 * @param q  Queue created with xQueueCreate(depth, sizeof(can_frame_t)).
 */
void can_service_add_rx_observer(QueueHandle_t q);
