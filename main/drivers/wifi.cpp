#include "drivers/wifi.hpp"

#include <string.h>
#include <stdlib.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "psa/crypto.h"


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wifi";

static const char *wifi_event_name(int32_t id)
{
    switch (id) {
    case WIFI_EVENT_STA_START:       return "STA_START";
    case WIFI_EVENT_STA_STOP:        return "STA_STOP";
    case WIFI_EVENT_STA_CONNECTED:   return "STA_CONNECTED";
    case WIFI_EVENT_STA_DISCONNECTED: return "STA_DISCONNECTED";
    case WIFI_EVENT_STA_AUTHMODE_CHANGE: return "AUTHMODE_CHANGE";
    default:                          return "?";
    }
}

esp_err_t WiFi::scan_and_log(void)
{
    /* 用 = {} 整体清零,再逐字段赋值,避免 -Werror=missing-field-initializers */
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.ssid = NULL;
    scan_cfg.bssid = NULL;
    scan_cfg.channel = 0;
    scan_cfg.show_hidden = true;
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 100;
    scan_cfg.scan_time.active.max = 300;

    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(&scan_cfg, true), TAG, "scan_start failed");

    uint16_t count = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&count), TAG, "get_ap_num failed");
    ESP_LOGI(TAG, "扫描到 %u 个 AP", (unsigned)count);

    if (count > 0) {
        wifi_ap_record_t *recs = (wifi_ap_record_t *)malloc(count * sizeof(wifi_ap_record_t));
        if (recs != NULL) {
            uint16_t got = count;
            if (esp_wifi_scan_get_ap_records(&got, recs) == ESP_OK) {
                for (uint16_t i = 0; i < got; i++) {
                    ESP_LOGI(TAG, "AP[%u] ssid=%.32s rssi=%d ch=%d auth=%d",
                             (unsigned)i, recs[i].ssid, recs[i].rssi,
                             recs[i].primary, (int)recs[i].authmode);
                }
            }
            free(recs);
        }
    }
    return ESP_OK;
}

void WiFi::event_handler(void *arg, const char *event_base,
                         int32_t event_id, void *event_data)
{
    WiFi *self = static_cast<WiFi *>(arg);
    if (self == nullptr) {
        return;
    }

    if (event_base == WIFI_EVENT) {
        ESP_LOGI(TAG, "WIFI_EVENT: %s (%ld)", wifi_event_name(event_id), (long)event_id);

        if (event_id == WIFI_EVENT_STA_START) {
            /* 先关省电,再发起连接:
             * 省电的 PM 睡眠切换在 ESP32-S3 + IDF v6.0.1 上偶发崩溃(esp_timer 竞态);
             * 且连接过程中再改省电设置可能干扰连接流程。 */
            esp_err_t ps = esp_wifi_set_ps(WIFI_PS_NONE);
            ESP_LOGI(TAG, "esp_wifi_set_ps(WIFI_PS_NONE) = %s", esp_err_to_name(ps));

            esp_err_t ret = esp_wifi_connect();
            ESP_LOGI(TAG, "esp_wifi_connect() = %s", esp_err_to_name(ret));
        } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
            ESP_LOGI(TAG, "STA 已关联 AP");
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *ev =
                static_cast<wifi_event_sta_disconnected_t *>(event_data);
            ESP_LOGW(TAG, "断开, reason=%d", ev ? ev->reason : -1);
            self->m_connected = false;
            if (self->m_retry_count < WIFI_MAX_RETRY) {
                self->m_retry_count++;
                ESP_LOGW(TAG, "第 %d/%d 次重连...", self->m_retry_count, WIFI_MAX_RETRY);
                esp_err_t ret = esp_wifi_connect();
                ESP_LOGI(TAG, "esp_wifi_connect() = %s", esp_err_to_name(ret));
            } else {
                ESP_LOGE(TAG, "重连次数耗尽(%d),停止连接", WIFI_MAX_RETRY);
            }
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
    /* 0. 先初始化 PSA 加密库:
     * WPA2 四次握手会用 PSA(psa_import_key)算 HMAC-SHA1,
     * 若不先 psa_crypto_init,PSA 全局互斥锁未建好会在握手时崩溃
     * (xQueueSemaphoreTake: uxItemSize == 0 断言)。幂等,可重复调用。 */
    ESP_RETURN_ON_ERROR(psa_crypto_init(), TAG, "psa_crypto_init failed");

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

    /* 注:关闭省电已移到 STA_START 事件处理里、在 esp_wifi_connect() 之前执行,
     * 避免连接过程中再改省电设置导致连接停滞。 */
    ESP_LOGI(TAG, "WiFi STA 初始化完成,正在连接 %s ...", WIFI_SSID);
    return ESP_OK;
}
