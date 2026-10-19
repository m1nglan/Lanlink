/*
 * gateway_client.hpp — LanLink 语音网关 板端客户端 (ESP-IDF / esp_websocket_client)
 *
 * 用法:
 *   GatewayClient gw;
 *   gw.on_partial = [](const char* text){ ... };   // 可选
 *   gw.on_final   = [](const char* text){ ... };   // 可选
 *   gw.connect();                                  // 长连接, 内部自动重连
 *
 *   // 一轮识别 (按键按下):
 *   gw.start_round();
 *   // 循环: gw.send_audio(pcm, len);   // 16kHz/16bit/mono, 任意分块
 *   // 按键松开:
 *   gw.end_round();
 *   // 等 on_final 回调
 */
#pragma once

#include <functional>
#include <string>

#include "esp_websocket_client.h"

class GatewayClient {
public:
    /* 回调(可空) */
    std::function<void(const std::string &text)> on_partial;   // 中间结果
    std::function<void(const std::string &text)> on_final;     // 最终结果
    std::function<void(const std::string &msg)>  on_error;     // 错误

    GatewayClient() = default;
    ~GatewayClient();

    /* 连接服务器(长连接, 内部自动重连), uri 例: ws://39.104.84.177:18888/iat?token=lanlink2026 */
    esp_err_t connect(const char *uri);

    /* 断开 */
    void disconnect();

    bool is_connected() const { return m_connected; }

    /* ---- 一轮识别 ---- */

    /* 开始触发器: {"type":"start"} */
    esp_err_t start_round();

    /* 发送音频块 (PCM 16kHz/16bit/mono) */
    esp_err_t send_audio(const uint8_t *data, size_t len);

    /* 结束触发器: {"type":"end"} */
    esp_err_t end_round();

    /* 心跳: {"type":"ping"} */
    esp_err_t ping();

private:
    static void ws_event_handler(void *arg, esp_event_base_t base,
                                 int32_t event_id, void *data);
    void handle_event(int32_t event_id, void *data);
    esp_err_t send_text(const char *fmt, ...);

    esp_websocket_client_handle_t m_client = nullptr;
    bool m_connected = false;
};
