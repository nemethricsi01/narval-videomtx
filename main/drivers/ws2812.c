#include "drivers/ws2812.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "ws2812";

// RMT resolution: 10 MHz → 1 tick = 100 ns
// WS2812B timing:
//   bit0: T0H=400ns (4 ticks), T0L=900ns (9 ticks)
//   bit1: T1H=800ns (8 ticks), T1L=500ns (5 ticks)
//   reset: line low >50 µs (handled by idle gap between transmissions)

static rmt_channel_handle_t s_tx_chan  = NULL;
static rmt_encoder_handle_t s_encoder = NULL;

esp_err_t ws2812_init(int gpio_num)
{
    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num          = gpio_num,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&chan_cfg, &s_tx_chan), TAG, "tx channel");

    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .duration0 = 4, .level0 = 1, .duration1 = 9, .level1 = 0 },
        .bit1 = { .duration0 = 8, .level0 = 1, .duration1 = 5, .level1 = 0 },
        .flags.msb_first = 1,
    };
    ESP_RETURN_ON_ERROR(rmt_new_bytes_encoder(&enc_cfg, &s_encoder), TAG, "bytes encoder");
    ESP_RETURN_ON_ERROR(rmt_enable(s_tx_chan), TAG, "enable");
    return ESP_OK;
}

// WS2812 expects GRB byte order
esp_err_t ws2812_set(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t grb[3] = { g, r, b };
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    ESP_RETURN_ON_ERROR(
        rmt_transmit(s_tx_chan, s_encoder, grb, sizeof(grb), &tx_cfg),
        TAG, "transmit");
    return rmt_tx_wait_all_done(s_tx_chan, 50);
}

esp_err_t ws2812_off(void)
{
    return ws2812_set(0, 0, 0);
}

void ws2812_deinit(void)
{
    if (s_tx_chan) {
        rmt_disable(s_tx_chan);
        rmt_del_channel(s_tx_chan);
        s_tx_chan = NULL;
    }
    if (s_encoder) {
        rmt_del_encoder(s_encoder);
        s_encoder = NULL;
    }
}
