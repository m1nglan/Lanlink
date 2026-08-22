#pragma once

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

/*! 把编码器旋转方向发成 LVGL 键 (左→LV_KEY_LEFT, 右→LV_KEY_RIGHT)。
 * 发送给默认 group 的聚焦对象; 无默认 group 时仅警告一次并忽略。
 * 内部自带 lvgl_port_lock, 任意任务上下文都可调用。 */
void lvgl_port_send_encoder_dir(int dir);
