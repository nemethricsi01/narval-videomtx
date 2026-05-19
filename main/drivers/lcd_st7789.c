#include "drivers/lcd_st7789.h"
#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"

static const char *TAG = "lcd_st7789";

esp_err_t lcd_st7789_init(esp_lcd_panel_handle_t *out_panel,
                           esp_lcd_panel_io_handle_t *out_io)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num     = BOARD_PIN_LCD_SCLK,
        .mosi_io_num     = BOARD_PIN_LCD_MOSI,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = BOARD_LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = BOARD_PIN_LCD_DC,
        .cs_gpio_num       = BOARD_PIN_LCD_CS,
        .pclk_hz           = BOARD_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(BOARD_LCD_SPI_HOST, &io_cfg, out_io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_PIN_LCD_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(*out_io, &panel_cfg, out_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(*out_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(*out_panel));
    // Gap: this 1.9" panel's active columns start at offset 35 in the ST7789's
    // 240-column framebuffer. Applied per-rotation in the display service.
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(*out_panel, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(*out_panel, true));

    ESP_LOGI(TAG, "ready (%dx%d)", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return ESP_OK;
}
