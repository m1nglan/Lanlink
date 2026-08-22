#pragma once

#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

/* ================================================================
 * LCD 驱动: ST7789 320x170 1.9寸条屏 (SPI2_HOST, 纯 IDF esp_lcd)
 *
 * 接线:
 *   SCLK=40  MOSI=39  RST=38  DC=37  CS=36  BL(背光)=35
 *
 * 使用: lcd_panel_init() 只负责硬件初始化 + 点亮背光;
 *       LVGL 移植层(lvgl_port)拿到 panel/io 句柄做刷屏。
 * ================================================================ */

/* ------------------ 引脚 ------------------ */
#define LCD_PIN_SCLK    (GPIO_NUM_21)
#define LCD_PIN_MOSI    (GPIO_NUM_20)
#define LCD_PIN_RST     (GPIO_NUM_19)
#define LCD_PIN_DC      (GPIO_NUM_47)
#define LCD_PIN_CS      (GPIO_NUM_48)
#define LCD_PIN_BL      (GPIO_NUM_45)
#define LCD_BL_ON_LEVEL (1)                     /*!< 背光点亮电平 */

/* ------------------ 分辨率 / 时钟 ------------------ */
#define LCD_H_RES        (320)                  /*!< 条屏宽 */
#define LCD_V_RES        (170)                  /*!< 条屏高 */
#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)   /*!< SPI 像素时钟, 可试 40MHz */

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

/* ------------------ 旋转编码器预留引脚 ------------------
 * 后续接屏幕切换导航用 (本阶段只预留, 不初始化)。
 * 避开已占用: IO8/10/11/12/13 (按键+I2S) 和屏的 35~40 */
#define ENC_A_PIN       (GPIO_NUM_4)
#define ENC_B_PIN       (GPIO_NUM_5)
#define ENC_BTN_PIN     (GPIO_NUM_6)

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
