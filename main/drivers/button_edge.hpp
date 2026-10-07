#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "driver/gpio.h"

/* ================================================================
 * [链] GPIO双边沿中断 → button_edge_isr → (一次消抖 timer) button_edge_timer_cb
 *      → 【本驱动回调】 → on_button_edge(main.cpp)
 *
 * 事件回调式按键驱动(取代轮询): ANYEDGE 中断只推迟一个消抖 timer, 电平稳定后
 * 回调才读电平、与上次上报比较, 变了才上报 (天然支持 push-to-talk)。
 * 零任务 / 零轮询 / 不依赖 UI 存活; 回调跑在 esp_timer 任务上下文(普通任务,
 * 可安全投队列); 支持多按键(各自独立 timer)、按下与释放双沿。
 *
 * 用法: button_edge_init(BTN_REC_PIN, 0, on_btn, ctx);   // 实际引脚见 main.cpp
 * ================================================================ */

/* ------------------ 参数 ------------------ */
/*! 稳定多久算消抖完成 (抖动期不断被中断重启推迟)。
 *  50ms 是本硬件验证过不抖的值(旧轮询驱动 10ms×5 的等价物); 初版给 15ms 时
 *  一次按下会打出多条 按下/释放。代价: 按下延迟 ≈ 本值; 详见 MD/HANDOFF.md §5.3 */
#define BTN_EDGE_DEBOUNCE_MS   (50)
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

/*! 累计进入 GPIO 边沿 ISR 的次数 (诊断用)。
 *  ★ 判"按键线是不是在噪声里翻转": 正常按一次 = 几次~几十次; 几百/几千地涨 =
 *    引脚接触不良/悬空 → ISR 风暴 → 中断看门狗 panic。voice.cpp 每秒日志会打。 */
uint32_t button_edge_isr_count(void);
