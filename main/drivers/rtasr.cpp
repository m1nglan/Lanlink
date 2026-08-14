#include "drivers/rtasr.hpp"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_websocket_client.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "apikey.h"

static const char *TAG = "rtasr";

/* ======================= 类实现 ======================= */

esp_err_t RtAsr::init(void)
{
    if (m_ws != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 构建 ws:// URI(token 鉴权放 query) */
    char uri[160];
    snprintf(uri, sizeof(uri),
             "ws://%s:%d" RTASR_PATH "?token=%s",
             RTASR_HOST, RTASR_PORT, SERVER_TOKEN);

    esp_websocket_client_config_t cfg = {};
    cfg.uri = uri;
    cfg.buffer_size = 4096;                         /*!< 接收缓冲,容纳结果 JSON */
    cfg.network_timeout_ms = RTASR_CONNECT_TIMEOUT_MS;
    cfg.disable_auto_reconnect = true;              /*!< 会话不自动重连,断线交给上层重建 */

    m_ws = esp_websocket_client_init(&cfg);
    if (m_ws == NULL) {
        ESP_LOGE(TAG, "esp_websocket_client_init 失败");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_websocket_register_events(m_ws, WEBSOCKET_EVENT_ANY,
                                                  &RtAsr::ws_event_handler, this);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册事件失败: %s", esp_err_to_name(ret));
        esp_websocket_client_destroy(m_ws);
        m_ws = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "连接 %s ...", uri);
    ret = esp_websocket_client_start(m_ws);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动失败: %s", esp_err_to_name(ret));
        esp_websocket_client_destroy(m_ws);
        m_ws = NULL;
        return ret;
    }

    /* 等待 WebSocket 握手完成 */
    uint32_t deadline = (uint32_t)(esp_timer_get_time() / 1000) + RTASR_CONNECT_TIMEOUT_MS;
    while (!esp_websocket_client_is_connected(m_ws)) {
        if ((uint32_t)(esp_timer_get_time() / 1000) > deadline) {
            ESP_LOGE(TAG, "连接超时");
            esp_websocket_client_destroy(m_ws);
            m_ws = NULL;
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    m_connected = true;
    ESP_LOGI(TAG, "WebSocket 握手成功");
    return ESP_OK;
}

esp_err_t RtAsr::send_start(uint32_t timeout_ms)
{
    if (m_ws == NULL || !m_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *start = "{\"type\":\"start\"}";
    int sent = esp_websocket_client_send_text(m_ws, start, (int)strlen(start), timeout_ms);
    if (sent < 0) {
        ESP_LOGE(TAG, "发送 start 失败 ret=%d", sent);
        m_connected = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t RtAsr::send_audio(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (m_ws == NULL || !m_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    int sent = esp_websocket_client_send_bin(m_ws, (const char *)data, (int)len, timeout_ms);
    if (sent < 0) {
        ESP_LOGE(TAG, "发送音频失败 ret=%d", sent);
        m_connected = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t RtAsr::send_end(uint32_t timeout_ms)
{
    if (m_ws == NULL || !m_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *end = "{\"type\":\"end\"}";
    int sent = esp_websocket_client_send_text(m_ws, end, (int)strlen(end), timeout_ms);
    if (sent < 0) {
        m_connected = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void RtAsr::set_result_callback(rtasr_result_cb_t cb, void *user_ctx)
{
    m_cb = cb;
    m_cb_ctx = user_ctx;
}

void RtAsr::deinit(void)
{
    m_connected = false;
    if (m_ws != NULL) {
        esp_websocket_client_stop(m_ws);
        esp_websocket_client_destroy(m_ws);
        m_ws = NULL;
    }
}

void RtAsr::ws_event_handler(void *handler_args, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    (void)base;
    RtAsr *self = static_cast<RtAsr *>(handler_args);
    if (self == nullptr) {
        return;
    }

    esp_websocket_event_data_t *data = static_cast<esp_websocket_event_data_t *>(event_data);
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
        self->m_connected = true;
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        self->m_connected = false;
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WEBSOCKET_EVENT_ERROR");
        self->m_connected = false;
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data != nullptr && data->op_code == WS_TRANSPORT_OPCODES_TEXT) {
            self->handle_text(data->data_ptr, data->data_len);
        }
        break;
    default:
        break;
    }
}

void RtAsr::handle_text(const char *payload, int len)
{
    if (payload == NULL || len <= 0) {
        return;
    }

    /* esp_websocket_client 的 data_ptr 非 null 终止,按 len 拷贝并补 null 终止 */
    char buf[2048];
    if (len >= (int)sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    memcpy(buf, payload, len);
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        ESP_LOGW(TAG, "JSON 解析失败: %s", buf);
        return;
    }

    const cJSON *type_item = cJSON_GetObjectItem(root, "type");
    if (type_item == NULL || !cJSON_IsString(type_item)) {
        cJSON_Delete(root);
        return;
    }
    const char *type = type_item->valuestring;

    /* 错误帧 */
    if (strcmp(type, "error") == 0) {
        const cJSON *msg = cJSON_GetObjectItem(root, "message");
        ESP_LOGE(TAG, "网关错误: %s",
                 (msg != NULL && cJSON_IsString(msg)) ? msg->valuestring : "未知");
        cJSON_Delete(root);
        return;
    }

    /* 提取 text(partial / final) */
    const cJSON *text_item = cJSON_GetObjectItem(root, "text");
    if (text_item == NULL || !cJSON_IsString(text_item)) {
        cJSON_Delete(root);
        return;
    }
    const char *text = text_item->valuestring;

    bool is_final = (strcmp(type, "final") == 0);
    if (text[0] != '\0') {
        /* 中间结果同一行实时刷新(\r 回到行首覆盖),最终结果换行 */
        if (is_final) {
            printf("\r识别结果: %s\n", text);
        } else {
            printf("\r识别中: %s    ", text);
        }
        fflush(stdout);
        if (m_cb != NULL) {
            m_cb(text, is_final, m_cb_ctx);
        }
    }

    cJSON_Delete(root);
}
