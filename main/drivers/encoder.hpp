#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "driver/gpio.h"

/* ================================================================
 * 旋转编码器驱动 (正交 AB 相, 边沿中断 + 软件状态机, 单实例)
 *
 * 接线: 编码器 A 相 → GPIO7, B 相 → GPIO15 (公共端接 GND)
 * 旋转一圈分若干格(detent), 每格触发一次回调:
 *   左转 → ENC_DIR_LEFT, 右转 → ENC_DIR_RIGHT
 * 若实际旋转方向与命名相反, 交换 ENC_PIN_A/ENC_PIN_B 即可。
 *
 * LVGL 绑定: 方向经回调发成 LV_KEY_LEFT / LV_KEY_RIGHT (需加 lvgl_port_lock),
 *            见 main.cpp 的 encoder_lvgl_cb。
 * ================================================================ */

/* ------------------ 参数 ------------------ */
#define ENC_PIN_A       (GPIO_NUM_7)      /*!< A 相 (用户称"左") */
#define ENC_PIN_B       (GPIO_NUM_15)     /*!< B 相 (用户称"右") */
#define ENC_KEY_STEP    (4)               /*!< 累计多少状态跳变算一格 (EC11 一格≈4)。
                                              转一格出多个键 → 调大; 转几格才出一个键 → 调小 */
#define ENC_QUEUE_LEN   (16)
#define ENC_TASK_STACK  (4096)
#define ENC_TASK_PRIO   (3)
#define ENC_TASK_CORE   (1)               /*!< 与 LVGL 同核, 降低跨核锁开销 */

/*! 旋转方向 */
typedef enum {
    ENC_DIR_LEFT  = -1,   /*!< 左转(逆时针) */
    ENC_DIR_RIGHT =  1,   /*!< 右转(顺时针) */
} encoder_dir_t;

/*! 方向回调, 在编码器内部任务上下文调用 (须尽快返回, LVGL 绑定需加锁) */
typedef void (*encoder_cb_t)(encoder_dir_t dir);

/*! 初始化编码器 (GPIO 输入+上拉+双边沿中断 + 内部任务) */
esp_err_t encoder_init(void);

/*! 注册方向回调 (旋转一格 detent 触发一次) */
void encoder_set_callback(encoder_cb_t cb);
