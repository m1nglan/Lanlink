#pragma once

#include "esp_err.h"
#include "driver/gpio.h"

/* ================================================================
 * [链] (encoder_init | button_edge_init) → 【gpio_isr_service_ensure】 → 中断服务就绪
 *
 * GPIO 中断服务是全芯片唯一资源, 只能成功安装一次。encoder 和 button_edge 都要装
 * → 第二次会被 IDF 判为 "already installed" (gpio.c:537) 并打一条 **E 级日志**,
 *   看着像启动出错其实完全正常, 会误导排查 → 统一走本 helper 消掉它。
 * ★ 函数内 static 在 C++ 里保证全程序唯一实例(即使 inline 被内联进多个 .cpp),
 *   所以不依赖两个驱动谁先初始化。
 * ⚠️ 只在初始化阶段(单线程)调用, 未加锁。
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
