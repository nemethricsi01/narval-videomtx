#include "drivers/video_uart.h"
#include "board.h"
#include "driver/uart.h"
#include "esp_log.h"

static const char *TAG = "video_uart";

esp_err_t video_uart_init(void)
{
    const uart_config_t cfg = {
        .baud_rate  = BOARD_VIDEO_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(BOARD_VIDEO_UART_PORT, 256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(BOARD_VIDEO_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(BOARD_VIDEO_UART_PORT, BOARD_PIN_VIDEO_UART_TX,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "ready (TX=%d, %d baud)", BOARD_PIN_VIDEO_UART_TX, BOARD_VIDEO_UART_BAUD);
    return ESP_OK;
}

void video_uart_send(uint8_t addr, uint8_t channel)
{
    uint8_t frame[4] = { 0x55, addr, channel, 0xAA };
    uart_write_bytes(BOARD_VIDEO_UART_PORT, (const char *)frame, sizeof(frame));
}
