#pragma once

#include "esp_err.h"
#include <stdint.h>

#define VIDEOMTX_IC_OUTPUTS      16
#define VIDEOMTX_IC_FRAME_BYTES  10  // 16 outputs * 5 bits (1 enable + 4 source) / 8

/**
 * Configure the CS/UPDATE GPIOs and add the crosspoint switch as a device on
 * BOARD_MTX_SPI_HOST (initializes that bus — see board.h for sharing notes).
 * Call once at startup, before any videomtx_ic_write().
 */
esp_err_t videomtx_ic_init(void);

/**
 * Push the full 16-output routing table to the crosspoint IC in one
 * load+latch sequence. routing[out] = input (0..15) for output `out`;
 * every output is sent enabled (no per-output mute yet).
 * Blocks for ~2 ms. Safe to call from any task.
 */
esp_err_t videomtx_ic_write(const uint8_t routing[VIDEOMTX_IC_OUTPUTS]);
