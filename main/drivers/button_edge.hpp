#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "driver/gpio.h"

/* ================================================================
 * 按键边沿驱动 (GPIO 中断 + esp_timer 软件消抖, 事件回调式, 取代轮询)
 *
 * 原理:
 *   按下/松开/抖动都触发 ANYEDGE 中断 → ISR 只重启一个 ~15ms 一次性 esp_timer
 *   → 电平稳定 15ms 无新中断后, timer 回调读稳定电平, 与上次上报比较
 *   → 变了才触发对应"按下/释放"回调 (天然支持 push-to-talk)
 *
 * 特性:
 *   - 零轮询: 不占任务, 不依赖任何 UI 存活
 *   - 回调运行在 esp_timer 任务上下文 (普通任务, 可安全调 xQueueSend 等)
 *   - 支持多按键 (各自独立 timer), 按下/释放双沿
 *
 * 使用:
 *   button_edge_init(GPIO_NUM_10, 0, on_btn, ctx);   // active_level=0 按下为低
 *   ...
 *   static void on_btn(void *ctx, gpio_num_t pin, bool pressed) { ... }
 * ================================================================ */

/* ------------------ 参数 ------------------ */
#define BTN_EDGE_DEBOUNCE_MS   (15)   /*!< 稳定多久算消抖完成 (抖动期不断被中断重启推迟) */
#define BTN_EDGE_MAX_BUTTONS   (4)    /*!< 最多支持几个按键 */

/*! 边沿回调: pressed=true 按下沿, false 释放沿。运行在 esp_timer 任务上下文。 */
typedef void (*btn_edge_cb_t)(void *ctx, gpio_num_t pin, bool pressed);

/*!
 * 初始化一个按键 (GPIO 输入 + 上拉/下拉 + 双边沿中断 + 独立消抖 timer)。
 * @param pin           按键 GPIO
 * @param active_level  按下时的电平 (0=按下为低/上拉, 1=按下为高)
 * @param cb            边沿回调 (不可为 NULL)
 * @param ctx           回调透传上下文
 * @return ESP_OK 成功
 */
esp_err_t button_edge_init(gpio_num_t pin, int active_level, btn_edge_cb_t cb, void *ctx);
