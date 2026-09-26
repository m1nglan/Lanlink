#pragma once

#include "esp_err.h"
#include "driver/gpio.h"

/* ================================================================
 * GPIO 中断服务: 全芯片唯一资源, 只能成功安装一次
 *
 * 问题: encoder 和 button_edge 都需要 GPIO 中断, 各自调一次
 *       gpio_install_isr_service() 时, 第二次会被 IDF 判为
 *       "GPIO isr service already installed" (gpio.c:537)
 *       并打出一条 **ESP_LOGE → E 级日志**。
 *       调用方虽然能靠 ESP_ERR_INVALID_STATE 容忍, 但启动日志里出现
 *       一条 E 会误导后续排查(看着像启动出错, 其实完全正常)。
 *
 * 方案: 两个驱动统一走本 helper。谁先调谁装, 后调的直接返回 ESP_OK,
 *       不再产生那条 E。函数内 static 在 C++ 里保证**全程序唯一实例**
 *       (即使本 inline 函数被内联进多个 .cpp), 所以不依赖各驱动的
 *       初始化顺序 —— 谁先谁后都对。
 *
 * 注意: 只在初始化阶段(单线程)调用, 未加锁。
 * ================================================================ */

/*! 确保 GPIO ISR 服务已安装(幂等)。返回 ESP_OK 表示"服务可用"。 */
inline esp_err_t gpio_isr_service_ensure(void)
{
    static bool s_installed = false;
    if (s_installed) {
        return ESP_OK;
    }

    esp_err_t ret = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
    if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
        /* ESP_ERR_INVALID_STATE = 别人已经装好了, 同样可用 */
        s_installed = true;
        return ESP_OK;
    }
    return ret;
}
