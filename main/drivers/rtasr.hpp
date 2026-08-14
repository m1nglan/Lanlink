#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ============ LanLink 语音网关(自建转发服务) 参数 ============ */
#define RTASR_HOST               "39.104.84.177"
#define RTASR_PORT               (18888)
#define RTASR_PATH               "/iat"
#define RTASR_CONNECT_TIMEOUT_MS (15000)      /*!< 连接超时 */
#define RTASR_SEND_TIMEOUT_MS    (1000)       /*!< 发送超时 */

/* 识别结果回调(在 esp_websocket_client 事件回调上下文调用,勿做阻塞操作)
 * text: 本次识别文本; is_final: true=最终结果; user_ctx: 自定义参数 */
typedef void (*rtasr_result_cb_t)(const char *text, bool is_final, void *user_ctx);

/*!
 * 语音网关客户端(基于官方 esp_websocket_client,直接发原始 PCM,鉴权在服务端)
 * 使用示例:
 *   RtAsr asr;
 *   asr.init();                       // 连接 ws://gateway/iat?token=...
 *   asr.send_audio(pcm, 1280, 1000);  // 每 40ms 发 1280 字节裸 PCM
 *   asr.send_end(1000);               // 发送结束帧 {"type":"end"}
 */
class RtAsr {
public:
    esp_err_t init(void);                                   /*!< 连接网关 */
    esp_err_t send_start(uint32_t timeout_ms);              /*!< 发送开始帧 {"type":"start"} */
    esp_err_t send_audio(const uint8_t *data, size_t len, uint32_t timeout_ms);  /*!< 发送二进制音频帧 */
    esp_err_t send_end(uint32_t timeout_ms);                /*!< 发送结束帧 {"type":"end"} */
    void deinit(void);                                      /*!< 断开并释放 */
    bool is_connected(void) const { return m_connected; }
    void set_result_callback(rtasr_result_cb_t cb, void *user_ctx);

private:
    /* esp_websocket_client 事件回调(静态,签名与 esp_event_handler_t 一致) */
    static void ws_event_handler(void *handler_args, esp_event_base_t base,
                                 int32_t event_id, void *event_data);
    void handle_text(const char *payload, int len);

    esp_websocket_client_handle_t m_ws = NULL;   /*!< esp_websocket_client 句柄 */
    volatile bool m_connected = false;
    rtasr_result_cb_t m_cb = NULL;
    void *m_cb_ctx = NULL;
};
