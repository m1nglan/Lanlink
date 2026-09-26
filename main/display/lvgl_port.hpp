#pragma once

#include <stdint.h>

#include "esp_err.h"

/* ================================================================
 * LVGL 9.5 移植层 (esp_lcd 后端)
 *
 * 职责: lv_init + 创建 320x170 display + 绘图缓冲 + flush 回调
 *       + 2ms tick 定时器 + LVGL 任务(CPU1)。
 *
 * 线程安全: LVGL 非线程安全, 其他任务访问 UI 必须:
 *     lvgl_port_lock();   ... 改 UI ...;   lvgl_port_unlock();
 * ================================================================ */

/*! 初始化 LCD + LVGL (含启动 LVGL 任务). 在 app_main 调用, 之后才能 ui_init() */
esp_err_t lvgl_port_init(void);

/*! 获取 LVGL 互斥锁 (阻塞) */
void lvgl_port_lock(void);

/*! 释放 LVGL 互斥锁 */
void lvgl_port_unlock(void);

/*! 注册编码器为 LVGL indev (LV_INDEV_TYPE_ENCODER)。
 * 需读取 drivers/encoder 的原始跳变 (encoder_consume_raw)。
 * ★ 必须在持有 lvgl_port_lock() 的情况下调用 (会创建 lv_indev + 默认 group)。
 * 注册后: 旋转由 lv_timer_handler 内部自动读 → indev_encoder_proc 发 LV_KEY_LEFT/RIGHT,
 * 不再需要任何"从任务里手动注入编码器方向"的入口。 */
esp_err_t lvgl_port_register_encoder_indev(void);
