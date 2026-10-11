#pragma once

#include <stdint.h>

#include "esp_err.h"

/* ================================================================
 * LVGL 9.5 移植层 (esp_lcd 后端)
 * 职责: lv_init + 320x170 display + 绘图缓冲 + flush 回调 + 2ms tick + LVGL 任务(CPU1)
 *
 * [链] lvgl_port_task(CPU1) → lvgl_port_lock → lv_timer_handler
 *        ├─ 定时器侧: 【ui_timer_cb → UiBridge::drain_queues】(消费队列 + 上屏)
 *        └─ 刷新侧:   lv_refr → 【flush_cb】 → ST7789
 *
 * 线程安全: LVGL 非线程安全 —— 其它任务改 UI 必须 lvgl_port_lock/unlock;
 *   ★★ 但 LVGL 上下文内 (回调 / lv_timer) 绝不能再加锁: 锁非递归, 会自死锁。
 * ================================================================ */

/*! 初始化 LCD + LVGL + 启动 LVGL 任务。app_main 调用, 之后才能 ui_init() */
esp_err_t lvgl_port_init(void);

void lvgl_port_lock(void);      /*!< 取 LVGL 互斥锁 (阻塞) */
void lvgl_port_unlock(void);    /*!< 放 LVGL 互斥锁 */

/*! 注册编码器为 LVGL indev。
 *  ★ 必须持 lvgl_port_lock() 调用 (会建 lv_indev + 默认 group)。
 *  注册后旋转由 lv_timer_handler 内部自动读, 不再需要"从任务里手动注入方向"的入口。 */
esp_err_t lvgl_port_register_encoder_indev(void);

/*! 暂停 / 恢复编码器输入 (false=暂停) —— 录音期间禁止转屏。
 *  ★★ 暂停时【照样取走并清零】编码器累加器: 攒着的话恢复那一刻会一次性倾泻, 连跳几屏。
 *     语义是"暂停期间转的不算数", 不是"先存着待会儿用"。详见 lvgl_encoder_read_cb()。
 *  ★ 只写一个 bool, 任意任务可调, 不用持 LVGL 锁。 */
void lvgl_port_set_encoder_enabled(bool enabled);
