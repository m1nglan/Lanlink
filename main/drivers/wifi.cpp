#include "drivers/wifi.hpp"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

static const char *TAG = "wifi";

void WiFi::event_handler(void *arg, const char *event_base,
                         int32_t event_id, void *event_data)
{
    WiFi *self = static_cast<WiFi *>(arg);
    if (self == nullptr) {
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        /* STA 启动完成,发起连接 */
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        self->m_connected = false;
        if (self->m_retry_count < WIFI_MAX_RETRY) {
            self->m_retry_count++;
            ESP_LOGW(TAG, "连接断开,第 %d/%d 次重连...", self->m_retry_count, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "重连次数耗尽(%d),停止连接", WIFI_MAX_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = static_cast<ip_event_got_ip_t *>(event_data);
        ESP_LOGI(TAG, "连接成功,IP: " IPSTR, IP2STR(&event->ip_info.ip));
        self->m_connected = true;
        self->m_retry_count = 0;
    }
}

esp_err_t WiFi::init(void)
{
    /* 1. 初始化 NVS(esp_wifi 依赖) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs_flash_init failed");

    /* 2. 网络接口 + 事件循环 + 默认 STA netif */
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "esp_event_loop_create_default failed");
    esp_netif_create_default_wifi_sta();

    /* 3. 初始化 WiFi 底层 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");

    /* 4. 注册事件回调(传入 this,回调里用它更新状态) */
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WiFi::event_handler, this),
        TAG, "register WIFI_EVENT failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WiFi::event_handler, this),
        TAG, "register IP_EVENT failed");

    /* 5. 配置 STA 并启动 */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "esp_wifi_set_storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode failed");

    wifi_config_t wifi_cfg = {};
    strlcpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "esp_wifi_set_config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");

    ESP_LOGI(TAG, "WiFi STA 初始化完成,正在连接 %s ...", WIFI_SSID);
    return ESP_OK;
}
