#pragma once

#include "esp_err.h"
#include <stdint.h>

/**
 * Configure BOARD_VIDEO_UART_PORT for TX-only output on
 * BOARD_PIN_VIDEO_UART_TX. Call once at startup.
 */
esp_err_t video_uart_init(void);

/**
 * Send one crosspoint update as [0x55][addr][channel][0xAA] — mirrors the
 * legacy AddSerialVideo() framing from the Vígszínház project. Runs
 * independently of, and in parallel with, the SPI crosspoint IC.
 */
void video_uart_send(uint8_t addr, uint8_t channel);
