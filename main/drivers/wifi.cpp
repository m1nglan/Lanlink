#include "drivers/wifi.hpp"

#include <string.h>
#include <stdlib.h>          /* setenv */
#include <time.h>            /* tzset */

#include "esp_check.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"  /* esp_netif_sntp_init: IDF 自带的一站式 SNTP */
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi";

/* 事件组 bit:连接成功 / 失败(重试耗尽) */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* 事件组句柄(事件回调与 init 间共享,官方例程同款机制) */
static EventGroupHandle_t s_wifi_event_group;

/* NTP 服务器 (国内可达的阿里云 NTP) 与时区 */
#define NTP_SERVER  "ntp.aliyun.com"
#define NTP_TZ      "CST-8"        /* 北京时间 (UTC+8, 无夏令时) */

/*! 启动 SNTP —— 由 IP_EVENT_STA_GOT_IP 触发, **有网才起**。
 *
 *  ★ 为什么不在 app_main 里起: SNTP 要发 UDP 请求, 没拿到 IP 之前发不出去,
 *    早起了只会白等一轮超时。
 *  ★ 为什么用 esp_netif_sntp 而不是老的 esp_sntp_*:
 *    前者是 IDF 5.x+ 的一站式封装 (建 netif 连接 + 服务器列表 + 同步等待),
 *    一个 esp_netif_sntp_init 就够了, 不用自己搭 socket 回调。
 *  ★ 时区必须【在同步之前】设好 —— 否则 synced 之后拿到的是 UTC,
 *    要等到下一次 localtime_r 才按 TZ 换算。
 *  ★ 只在【第一次】拿到 IP 时起 (断线重连会再次进 GOT_IP, 重复 init 会报错)。 */
static void sntp_start_once(void)
{
    static bool started = false;
    if (started) {
        return;
    }
    started = true;

    setenv("TZ", NTP_TZ, 1);
    tzset();

    /* ESP_NETIF_SNTP_DEFAULT_CONFIG 是 C 的指定初始化器, 但字段顺序与声明一致,
     * C++20 允许 (本项目用 gnu++26) → 可以直接用。 */
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP 启动失败: %s", esp_err_to_name(err));
        started = false;              /* 允许下次 GOT_IP 再试 */
        return;
    }
    ESP_LOGI(TAG, "SNTP 已启动 (%s, TZ=%s), 后台自动同步", NTP_SERVER, NTP_TZ);
}

void WiFi::event_handler(void *arg, const char *event_base,
                         int32_t event_id, void *event_data)
{
    WiFi *self = static_cast<WiFi *>(arg);

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (self != nullptr && self->m_retry_count < WIFI_MAX_RETRY) {
            self->m_retry_count++;
            esp_wifi_connect();
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            if (s_wifi_event_group != NULL) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
            ESP_LOGI(TAG, "connect to the AP fail");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = static_cast<ip_event_got_ip_t *>(event_data);
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        if (self != nullptr) {
            self->m_connected = true;
            self->m_retry_count = 0;
        }
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        /* ★ 拿到 IP → 有网了, 起 SNTP 对时 (只起一次) */
        sntp_start_once();
    }
}

esp_err_t WiFi::init(void)
{
    /* 事件组:用于阻塞等待连接结果(成功 / 重试耗尽) */
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 初始化 NVS(esp_wifi 依赖) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs_flash_init failed");

    /* 网络接口 + 事件循环 + 默认 STA netif */
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "esp_event_loop_create_default failed");
    esp_netif_create_default_wifi_sta();

    /* 初始化 WiFi 底层 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");

    /* 注册事件回调 */
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WiFi::event_handler, this),
        TAG, "register WIFI_EVENT failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WiFi::event_handler, this),
        TAG, "register IP_EVENT failed");

    /* 配置 STA 并启动 */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "esp_wifi_set_storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode failed");

    wifi_config_t wifi_cfg = {};
    strlcpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "esp_wifi_set_config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");

    ESP_LOGI(TAG, "wifi_init_sta finished, 等待连接 %s ...", WIFI_SSID);

    /* 阻塞等待:连接成功或重试耗尽 */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", WIFI_SSID);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Failed to connect to SSID:%s", WIFI_SSID);
    return ESP_FAIL;
}
