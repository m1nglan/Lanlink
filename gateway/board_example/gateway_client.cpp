/*
 * gateway_client.cpp — LanLink 语音网关 板端客户端实现
 */
#include "gateway_client.hpp"

#include <cstdarg>
#include <cstdio>

#include "esp_log.h"

static const char *TAG = "gw_client";

GatewayClient::~GatewayClient()
{
    disconnect();
}

/* 事件回调(静态) */
void GatewayClient::ws_event_handler(void *arg, esp_event_base_t base,
                                     int32_t event_id, void *data)
{
    auto *self = static_cast<GatewayClient *>(arg);
    if (self) {
        self->handle_event(event_id, data);
    }
}

void GatewayClient::handle_event(int32_t event_id, void *data)
{
    auto *event = static_cast<esp_websocket_event_data_t *>(data);

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        m_connected = true;
        ESP_LOGI(TAG, "connected to gateway");
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        m_connected = false;
        ESP_LOGW(TAG, "disconnected (will auto-reconnect)");
        break;

    case WEBSOCKET_EVENT_DATA:
        if (event->op_code == 0x1 /* text */ && event->data_len > 0) {
            std::string msg(event->data_ptr, event->data_len);
            parse_server_msg(msg);
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "websocket error");
        break;

    default:
        break;
    }
}

/* 解析服务器下行 JSON: partial / final / error / pong */
void GatewayClient::parse_server_msg(const std::string &msg)
{
    // 极简 JSON 解析(够用即可, 避免引入 cJSON 依赖的话可手写;
    // 项目里若已用 cJSON 建议换 cJSON_Parse)
    auto find_field = [&msg](const char *key) -> std::string {
        std::string pat = "\"" + std::string(key) + "\"";
        size_t pos = msg.find(pat);
        if (pos == std::string::npos) return "";
        size_t v = msg.find('"', pos + pat.size());
        if (v == std::string::npos) return "";
        size_t v2 = msg.find('"', v + 1);
        if (v2 == std::string::npos) return "";
        return msg.substr(v + 1, v2 - v - 1);
    };

    std::string type = find_field("type");

    if (type == "partial") {
        if (on_partial) on_partial(find_field("text"));
    } else if (type == "final") {
        if (on_final) on_final(find_field("text"));
    } else if (type == "error") {
        if (on_error) on_error(find_field("message"));
    } else if (type == "pong") {
        ESP_LOGD(TAG, "pong");
    }
}

esp_err_t GatewayClient::connect(const char *uri)
{
    if (m_client) {
        esp_websocket_client_destroy(m_client);
        m_client = nullptr;
    }
    esp_websocket_client_config_t cfg = {};
    cfg.uri = uri;
    cfg.reconnect_timeout_ms = 3000;   // 3s 自动重连
    cfg.network_timeout_ms = 5000;
    m_client = esp_websocket_client_init(&cfg);
    if (!m_client) {
        return ESP_ERR_NO_MEM;
    }
    esp_websocket_register_events(m_client, WEBSOCKET_EVENT_ANY,
                                  &GatewayClient::ws_event_handler, this);
    return esp_websocket_client_start(m_client);
}

void GatewayClient::disconnect()
{
    if (m_client) {
        esp_websocket_client_stop(m_client);
        esp_websocket_client_destroy(m_client);
        m_client = nullptr;
        m_connected = false;
    }
}

esp_err_t GatewayClient::send_text(const char *fmt, ...)
{
    if (!m_client || !m_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return esp_websocket_client_send_text(m_client, buf, strlen(buf), pdMS_TO_TICKS(2000));
}

esp_err_t GatewayClient::start_round()
{
    return send_text("{\"type\":\"start\"}");
}

esp_err_t GatewayClient::send_audio(const uint8_t *data, size_t len)
{
    if (!m_client || !m_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_websocket_client_send_bin(m_client, (const char *)data, len, pdMS_TO_TICKS(2000));
}

esp_err_t GatewayClient::end_round()
{
    return send_text("{\"type\":\"end\"}");
}

esp_err_t GatewayClient::ping()
{
    return send_text("{\"type\":\"ping\"}");
}
