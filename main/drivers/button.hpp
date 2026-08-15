#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "driver/gpio.h"

/* ------------------ 默认参数(按实际修改) ------------------ */
#define BTN_ACTIVE_LEVEL    (0)                  /*!< 默认按下电平:1=按下为高(外部下拉),0=按下为低(上拉) */
#define BTN_PULL_UP_EN      (GPIO_PULLUP_ENABLE)   /*!< 内部上拉:外部下拉接法用 DISABLE */
#define BTN_PULL_DOWN_EN    (GPIO_PULLDOWN_DISABLE) /*!< 内部下拉:外部下拉接法用 DISABLE */
#define BTN_DEBOUNCE_MS     (10)                 /*!< 采样间隔(≥10ms,CONFIG_FREERTOS_HZ=100 时 1 tick=10ms) */
#define BTN_DEBOUNCE_N      (5)                  /*!< 稳定采样次数(总消抖约 50ms) */

/*!
 * 标准按键驱动(数字输入 + 软件消抖),支持多实例
 * 使用示例:
 *   Button btn1(GPIO_NUM_12);
 *   Button btn2(GPIO_NUM_13);
 *   btn1.init();
 *   btn2.init();
 *   while (1) {
 *       if (btn1.is_pressed()) { ... }
 *       if (btn2.is_pressed()) { ... }
 *   }
 */
class Button {
public:
    /*! 构造:传入按键引脚;active_level 为该按键"按下"对应的电平(默认按宏) */
    Button(gpio_num_t pin, int active_level = BTN_ACTIVE_LEVEL)
        : m_pin(pin), m_active_level(active_level) {}

    /*! 初始化:配置 GPIO 为输入 + 上下拉,并读取一次初始状态 */
    esp_err_t init(void);

    /*! 消抖读取,返回是否处于"按下"状态 */
    bool is_pressed(void);

    /*! 消抖读取,返回原始稳定电平(0 或 1) */
    int read_level(void);

private:
    gpio_num_t m_pin;           /*!< 本实例的按键引脚 */
    int m_active_level;         /*!< 本实例按下时对应的电平 */
    bool m_pressed = false;     /*!< 最近一次消抖后的按下状态 */
};
