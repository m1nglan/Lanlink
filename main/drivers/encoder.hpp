#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "driver/gpio.h"

/* ================================================================
 * [链] GPIO中断(A/B双相) → encoder_isr → s_accum → [lvgl任务] lvgl_encoder_read_cb
 *      → data->enc_diff → LVGL → LV_KEY_LEFT/RIGHT → 聚焦对象 → 冒泡到屏 → 切屏回调
 *
 * 正交 AB 相编码器: 无任务 / 无队列 / 无回调 —— ISR 解方向累加, 消费方主动取。
 * 归一化: 一格 detent ≈ ENC_KEY_STEP 个状态跳变, 消费方按此换算格数。
 * 方向反了就交换 ENC_PIN_A / ENC_PIN_B。
 * ================================================================ */

/* ------------------ 参数 ------------------ */
#define ENC_PIN_A       (GPIO_NUM_45)      /*!< A 相 (用户称"左") */
#define ENC_PIN_B       (GPIO_NUM_41)     /*!< B 相 (用户称"右") */
#define ENC_KEY_STEP    (4)               /*!< 一格 detent 的状态跳变数 (EC11 一格≈4)。
                                              转一格出多个键 → 调大; 转几格才出一个键 → 调小 */

/*! 初始化编码器: GPIO 输入+上拉+双边沿中断 + 挂 ISR (不创建任务) */
esp_err_t encoder_init(void);

/*! 取走自上次调用以来的净跳变数并清零 (原子, 可跨任务/ISR 安全调用)。
 *  正=顺时针(右), 负=逆时针(左)。消费方通常除以 ENC_KEY_STEP 得格数。 */
int encoder_consume_raw(void);
