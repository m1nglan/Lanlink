#pragma once

#include <stdint.h>

#include "esp_err.h"

/* ------------------ WiFi 参数(按实际修改) ------------------ */
#define WIFI_SSID           "CMCC-360"      /*!< 要连接的 WiFi SSID */
#define WIFI_PASSWORD       "meiyoumima"    /*!< WiFi 密码 */
#define WIFI_MAX_RETRY      (10)            /*!< 断开后最大重连次数 */

/*!
 * 基于官方 station 例程重构的 WiFi 驱动(STA 模式)
 * 使用示例:
 *   WiFi wifi;
 *   ESP_ERROR_CHECK(wifi.init());   // 阻塞,直到连上或重试耗尽
 *   // init 返回后可用 wifi.is_connected() 判断结果
 */
class WiFi {
public:
    /*! 初始化 NVS/netif/事件循环并以 STA 模式连接;阻塞至连接成功或重试耗尽 */
    esp_err_t init(void);

    /*! 是否已连接(init 返回后有效) */
    bool is_connected(void) const { return m_connected; }

    /*! 当前已重连次数 */
    int get_retry_count(void) const { return m_retry_count; }

private:
    /* 事件回调(静态成员,签名与 esp_event_handler_t 一致:event_base 即 const char*) */
    static void event_handler(void *arg, const char *event_base,
                              int32_t event_id, void *event_data);

    bool m_connected = false;   /*!< 是否已连接 */
    int m_retry_count = 0;      /*!< 重连计数 */
};
