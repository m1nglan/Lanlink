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

/*! 把编码器旋转方向发成 LVGL 键 (左→LV_KEY_LEFT, 右→LV_KEY_RIGHT)。
 * 自动创建/复用默认 group, 并把当前激活屏幕设为聚焦对象,
 * 使 SquareLine 挂在屏幕对象上的 LV_EVENT_KEY 切屏事件生效。
 * 内部自带 lvgl_port_lock, 任意任务上下文都可调用。 */
void lvgl_port_send_encoder_dir(int dir);

/*! 注册编码器为 LVGL indev (LV_INDEV_TYPE_ENCODER)。
 * 需读取 drivers/encoder 的原始跳变 (encoder_consume_raw)。
 * ★ 必须在持有 lvgl_port_lock() 的情况下调用 (会创建 lv_indev + 默认 group)。
 * 注册后: 旋转由 lv_timer_handler 内部自动读→驱动 group 导航/发键,
 * 无需再调 lvgl_port_send_encoder_dir。 */
esp_err_t lvgl_port_register_encoder_indev(void);

/*! 诊断: 编码器 read_cb 被 LVGL 调用的累计次数 (稳定后可删)。
 *  若长时间为 0, 说明 indev 未被 LVGL 轮询。 */
uint32_t lvgl_encoder_read_calls(void);

/*! 诊断: 累计交付给 group 的 |格数| (稳定后可删) */
uint32_t lvgl_encoder_steps_total(void);

/*! 诊断: 默认 group 是否编辑模式 (1=旋转会发 LV_KEY_LEFT/RIGHT)。
 *  ★ 为 0 时旋转只移动焦点, SquareLine 切屏事件收不到 → 表现=编码器无反应。 */
uint32_t lvgl_encoder_editing(void);

/*! 诊断: 聚焦对象是否就是当前激活屏 (1=是) */
uint32_t lvgl_encoder_focus_ok(void);
