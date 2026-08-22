#include "display/lcd_display.hpp"

#include <string.h>

#include "esp_log.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_st7789.h"
#include "driver/spi_master.h"

static const char *TAG = "lcd";

esp_err_t lcd_panel_init(esp_lcd_panel_handle_t *panel_out, esp_lcd_panel_io_handle_t *io_out)
{
    if (panel_out == NULL || io_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *panel_out = NULL;
    *io_out = NULL;

    /* 1. SPI 总线 (SPI2_HOST, 只有写: 无 MISO) */
    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num = LCD_PIN_SCLK;
    buscfg.mosi_io_num = LCD_PIN_MOSI;
    buscfg.miso_io_num = -1;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t);
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 2. panel IO (DC + CS, SPI 模式 0) */
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_gpio_num = LCD_PIN_DC;
    io_config.cs_gpio_num = LCD_PIN_CS;
    io_config.pclk_hz = LCD_PIXEL_CLOCK_HZ;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.spi_mode = 0;
    io_config.trans_queue_depth = 10;
    ret = esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 3. ST7789 面板 */
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = LCD_PIN_RST;
#if LCD_RGB_ORDER_RGB
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;   /* 本条屏用 RGB (BGR 会把黄显示成红) */
#else
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
#endif
    panel_config.bits_per_pixel = 16;
    ret = esp_lcd_new_panel_st7789(io, &panel_config, &panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_reset(panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_reset: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_lcd_panel_init(panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_init: %s", esp_err_to_name(ret));
        return ret;
    }
    /* 条屏方向/偏移 (bring-up 调参宏) */
    esp_lcd_panel_swap_xy(panel, LCD_SWAP_XY);
    esp_lcd_panel_mirror(panel, LCD_MIRROR_X, LCD_MIRROR_Y);
    esp_lcd_panel_set_gap(panel, LCD_X_GAP, LCD_Y_GAP);
#if LCD_INVERT_COLOR
    esp_lcd_panel_invert_color(panel, true);
#endif
    ret = esp_lcd_panel_disp_on_off(panel, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_disp_on_off: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 4. 背光 (开机先关闭, UI 初始化完成后再点亮, 避免启动过程白屏/雪花) */
    gpio_config_t bl_cfg = {};
    bl_cfg.pin_bit_mask = 1ULL << LCD_PIN_BL;
    bl_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&bl_cfg);
    gpio_set_level(LCD_PIN_BL, !LCD_BL_ON_LEVEL);

    *panel_out = panel;
    *io_out = io;
    ESP_LOGI(TAG, "ST7789 init OK: %dx%d @ %d Hz (swap=%d mirror=%d,%d gap=%d,%d)",
             LCD_H_RES, LCD_V_RES, LCD_PIXEL_CLOCK_HZ,
             LCD_SWAP_XY, LCD_MIRROR_X, LCD_MIRROR_Y, LCD_X_GAP, LCD_Y_GAP);
    return ESP_OK;
}

void lcd_panel_backlight_on(void)
{
    gpio_set_level(LCD_PIN_BL, LCD_BL_ON_LEVEL);
}

void lcd_panel_deinit(void)
{
    /* 本项目不调用 deinit。面板句柄由 lvgl_port 持有, 如需释放需传入句柄。
     * 这里只关背光, 避免空指针操作。 */
    gpio_set_level(LCD_PIN_BL, !LCD_BL_ON_LEVEL);
}
