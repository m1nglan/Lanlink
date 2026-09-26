#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "driver/gpio.h"

/* ================================================================
 * 旋转编码器驱动 (正交 AB 相, 边沿中断 + 原子累加器, 单实例)
 *
 * 接线: 编码器 A 相 → GPIO7, B 相 → GPIO15 (公共端接 GND)
 *
 * 架构 (无任务, 无队列, 无回调):
 *   ISR 解出方向 → atomic 累加到 s_accum
 *   消费方(通常在 LVGL 的 indev read_cb 里)调 encoder_consume_raw() 取走净跳变
 *
 * 归一化: 一格 detent ≈ ENC_KEY_STEP 个状态跳变。消费方按此换算格数。
 * 若实际旋转方向与命名相反, 交换 ENC_PIN_A/ENC_PIN_B 即可。
 * ================================================================ */

/* ------------------ 参数 ------------------ */
#define ENC_PIN_A       (GPIO_NUM_7)      /*!< A 相 (用户称"左") */
#define ENC_PIN_B       (GPIO_NUM_15)     /*!< B 相 (用户称"右") */
#define ENC_KEY_STEP    (4)               /*!< 一格 detent 的状态跳变数 (EC11 一格≈4)。
                                              转一格出多个键 → 调大; 转几格才出一个键 → 调小 */

/*! 旋转方向 */
typedef enum {
    ENC_DIR_LEFT  = -1,   /*!< 左转(逆时针) */
    ENC_DIR_RIGHT =  1,   /*!< 右转(顺时针) */
} encoder_dir_t;

/*! 初始化编码器: GPIO 输入+上拉+双边沿中断 + 挂 ISR (不创建任务) */
esp_err_t encoder_init(void);

/*! 取走自上次调用以来的净跳变数并清零 (原子, 可跨任务/ISR 安全调用)。
 *  正=顺时针(右), 负=逆时针(左)。消费方通常除以 ENC_KEY_STEP 得格数。 */
int encoder_consume_raw(void);
