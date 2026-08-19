#include "drivers/videomtx_ic.h"
#include "board.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "videomtx_ic";

static spi_device_handle_t s_spi;

esp_err_t videomtx_ic_init(void)
{



    if (BOARD_PIN_MTX_SCLK < 0 || BOARD_PIN_MTX_MOSI < 0 ||
        BOARD_PIN_MTX_CS < 0 || BOARD_PIN_MTX_UPDATE < 0 || BOARD_PIN_MTX_RESET < 0) {
        ESP_LOGE(TAG, "MTX SCLK/MOSI/CS/UPDATE/RESET pins not set in board.h");
        return ESP_ERR_INVALID_STATE;
    }

    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << BOARD_PIN_MTX_CS) | (1ULL << BOARD_PIN_MTX_UPDATE) | (1ULL << BOARD_PIN_MTX_RESET),
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));
    gpio_set_level(BOARD_PIN_MTX_CS, 1);      // idle deselected
    gpio_set_level(BOARD_PIN_MTX_UPDATE, 1);  // idle high, matches the IC's rest state
    gpio_set_level(BOARD_PIN_MTX_RESET, 1);   // idle high, matches the IC's rest state

    // Dedicated bus — no other device shares BOARD_MTX_SPI_HOST.
    spi_bus_config_t buscfg = {
        .sclk_io_num     = BOARD_PIN_MTX_SCLK,
        .mosi_io_num     = BOARD_PIN_MTX_MOSI,
        .miso_io_num     = BOARD_PIN_MTX_MISO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = VIDEOMTX_IC_FRAME_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_MTX_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = BOARD_MTX_CLOCK_HZ,
        .mode           = 0,
        .spics_io_num   = -1,  // CS is bit-banged manually in videomtx_ic_write()
        .queue_size     = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(BOARD_MTX_SPI_HOST, &dev_cfg, &s_spi));

    ESP_LOGI(TAG, "ready (CS=%d UPDATE=%d, %d Hz)",
             BOARD_PIN_MTX_CS, BOARD_PIN_MTX_UPDATE, BOARD_MTX_CLOCK_HZ);
    return ESP_OK;
}

// Pack routing[] into the 10-byte serial frame the IC expects:
// output 15 first (MSB), 5 bits/output (D4=enable, D3..D0=source), MSB-first.
static void pack(const uint8_t routing[VIDEOMTX_IC_OUTPUTS], uint8_t *buf)
{
    memset(buf, 0, VIDEOMTX_IC_FRAME_BYTES);
    uint16_t bit_pos = 0;

    for (int8_t out = 15; out >= 0; out--) {
        uint8_t field = (1U << 4) | (routing[out] & 0x0FU);  // always enabled
        for (int8_t b = 4; b >= 0; b--) {
            uint8_t bit = (field >> b) & 1U;
            buf[bit_pos >> 3] |= bit << (7U - (bit_pos & 7U));
            bit_pos++;
        }
    }
}

esp_err_t videomtx_ic_write(const uint8_t routing[VIDEOMTX_IC_OUTPUTS])
{
    uint8_t buf[VIDEOMTX_IC_FRAME_BYTES];
    pack(routing, buf);

    spi_transaction_t t = {
        .length    = VIDEOMTX_IC_FRAME_BYTES * 8,
        .tx_buffer = buf,
    };

    gpio_set_level(BOARD_PIN_MTX_UPDATE, 1);
    gpio_set_level(BOARD_PIN_MTX_CS, 0);
    esp_rom_delay_us(1000);

    esp_err_t ret = spi_device_polling_transmit(s_spi, &t);

    gpio_set_level(BOARD_PIN_MTX_UPDATE, 0);
    esp_rom_delay_us(1000);
    gpio_set_level(BOARD_PIN_MTX_UPDATE, 1);
    gpio_set_level(BOARD_PIN_MTX_CS, 1);

    if (ret != ESP_OK)
        ESP_LOGE(TAG, "SPI transmit failed: %s", esp_err_to_name(ret));
    return ret;
}
