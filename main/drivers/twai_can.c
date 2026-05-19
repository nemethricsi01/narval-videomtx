#include "drivers/twai_can.h"
#include "board.h"
#include "esp_twai_onchip.h"
#include "esp_log.h"

static const char *TAG = "twai_can";

esp_err_t twai_can_init(twai_node_handle_t *out_node)
{
    // Create node with a placeholder bitrate; the exact timing is applied
    // below via reconfig before enabling (new API cannot express non-standard
    // bitrates like 80 MHz / (584 * 14) ≈ 9784.7 bps through the basic config).
    twai_onchip_node_config_t cfg = {
        .io_cfg = {
            .tx                = BOARD_PIN_CAN_TX,
            .rx                = BOARD_PIN_CAN_RX,
            .quanta_clk_out    = -1,
            .bus_off_indicator = -1,
        },
        .bit_timing = {
            .bitrate = 10000,
        },
        .tx_queue_depth = 10,
        // Self-test: the controller self-ACKs its own frames, so the TX queue
        // drains without needing an external receiver.  Frames are still sent
        // on the bus and received normally by any real node.  The controller
        // also loops transmitted frames back to its own RX FIFO — those are
        // filtered out in on_rx_done() in can.c.
        .flags = {
            .enable_self_test = 1,
        },
    };
    ESP_ERROR_CHECK(twai_new_node_onchip(&cfg, out_node));

    // Override with exact BRP/TSEG values from the original config.
    // prop_seg = 0: legacy tseg_1 already captured the full phase-1 segment.
    const twai_timing_advanced_config_t timing = {
        .brp     = 584,
        .prop_seg = 0,
        .tseg_1  = 7,
        .tseg_2  = 6,
        .sjw     = 1,
    };
    ESP_ERROR_CHECK(twai_node_reconfig_timing(*out_node, &timing, NULL));

    // Node is left stopped so the caller can register callbacks before enabling.
    ESP_LOGI(TAG, "configured (80 MHz / (584 * 14) ≈ 9784 bps, tx_queue=10)");
    return ESP_OK;
}
