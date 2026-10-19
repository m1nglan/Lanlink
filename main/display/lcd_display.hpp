#pragma once

#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

/* ================================================================
 * LCD 驱动: ST7789 320x170 1.9寸条屏 (SPI2_HOST, 纯 IDF esp_lcd)
 *
 * [链] app_main → lvgl_port_init ─┬─ 【lcd_panel_init】 → SPI总线 + panel IO + ST7789 + 背光先关
 *                                 └─ lv_init + display + flush_cb + tick + lvgl 任务
 *      ui_init 完成后 → 【lcd_panel_backlight_on】点亮背光 (避免启动过程白屏/雪花)
 *
 * ★★ 接线【以本文件下方 LCD_PIN_* 宏为唯一准绳】, 注释里的数字容易过时。
 *   旧版曾用 35/36/37 —— 撞 n16r8 的 Octal PSRAM(GPIO33-37), 出过雪花 + 位翻转;
 *   又用过 21/20/19/47/48/45; 现为下面这组。
 *   ⚠️ 其中 3/46 是 strapping 脚 (JTAG 源选择 / VDD_SPI), 合法但上电有默认电平。
 * ================================================================ */

/* ------------------ 引脚 ------------------ */
#define LCD_PIN_SCLK    (GPIO_NUM_9)
#define LCD_PIN_MOSI    (GPIO_NUM_46)
#define LCD_PIN_RST     (GPIO_NUM_3)
#define LCD_PIN_DC      (GPIO_NUM_8)
#define LCD_PIN_CS      (GPIO_NUM_18)
#define LCD_PIN_BL      (GPIO_NUM_17)
#define LCD_BL_ON_LEVEL (1)                     /*!< 背光点亮电平 */

/* ------------------ 分辨率 / 时钟 ------------------ */
#define LCD_H_RES        (320)                  /*!< 条屏宽 */
#define LCD_V_RES        (170)                  /*!< 条屏高 */
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)   /*!< SPI 像素时钟 40MHz */

/* ------------------ 条屏 bring-up 调参旋钮 ------------------
 * 320x170 条屏首次上电可能出现偏移/翻转/反色, 按实际现象调这里:
 *   SWAP_XY:    行列交换 (面板内部像素排列与显示方向)
 *   MIRROR_X/Y: 水平/垂直镜像
 *   X_GAP/Y_GAP: 可见区在面板矩阵内的偏移 (esp_lcd_panel_set_gap)
 * 颜色反相用 esp_lcd_panel_invert_color() 在 lcd_display.cpp 里开/关  */
#define LCD_SWAP_XY     (true)
#define LCD_MIRROR_X    (false)
#define LCD_MIRROR_Y    (true)
#define LCD_X_GAP       (0)
#define LCD_Y_GAP       (35)
/*  颜色调参 (黄显示成红/蓝互换/整体偏色时按此调):
 *   LCD_RGB_ORDER_RGB: 1=RGB 顺序, 0=BGR (红蓝通道交换)
 *   LCD_SWAP_BYTES:    1=flush 时交换 16bit 像素高低字节, 0=不交换
 *   LCD_INVERT_COLOR:  1=颜色反相 (部分 ST7789 条屏是负显)
 *   当前默认 RGB+交换字节, 已从 BGR 改 RGB 修复"黄色变红";
 *   若仍偏色, 优先试 LCD_SWAP_BYTES=0, 再试 LCD_INVERT_COLOR=1 */
#define LCD_RGB_ORDER_RGB   1
#define LCD_SWAP_BYTES      1
#define LCD_INVERT_COLOR    1

/*!
 * 初始化 SPI 总线 + ST7789 面板, 点亮背光。
 * @param[out] panel_out 面板句柄 (供 LVGL flush 用)
 * @param[out] io_out    panel io 句柄 (供注册 flush 完成事件用)
 */
esp_err_t lcd_panel_init(esp_lcd_panel_handle_t *panel_out, esp_lcd_panel_io_handle_t *io_out);

/*! 点亮背光 (UI 初始化完成后调用, 避免启动过程白屏/雪花) */
void lcd_panel_backlight_on(void);

/*! 释放面板 + SPI 总线 (通常不调用) */
void lcd_panel_deinit(void);
