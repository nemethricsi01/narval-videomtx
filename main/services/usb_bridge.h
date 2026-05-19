#pragma once

#include "esp_err.h"
#include <stdint.h>

/**
 * Initialise the CAN-USB CDC bridge.
 *
 * Must be called after can_service_init().  Registers an RX observer on the
 * CAN service, creates the USB CDC device, and spawns the bridge tasks.
 */
esp_err_t usb_bridge_init(void);

/* Pass as timeout_ms to block until a byte arrives. */
#define CDC_WAIT_FOREVER  0xFFFFFFFFu

/**
 * Read one byte from the USB CDC RX stream.
 * Must be called only from the usb_to_can_task context (not thread-safe).
 *
 * @param byte        Output byte.
 * @param timeout_ms  Milliseconds to wait; 0 = non-blocking; CDC_WAIT_FOREVER = block indefinitely.
 * @return 1 if a byte was returned, 0 on timeout.
 */
int cdc_read_byte(uint8_t *byte, unsigned int timeout_ms);

/**
 * Write len bytes to the USB CDC TX stream.
 * Copies buf into an internal DMA buffer, so buf need not be DMA-aligned.
 * Blocks up to 500 ms waiting for a previous write to finish.
 *
 * @return 1 on success, 0 if USB is not ready or the write timed out.
 */
int cdc_write(const uint8_t *buf, unsigned int len);
