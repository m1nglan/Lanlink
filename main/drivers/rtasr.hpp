#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ============ 讯飞实时语音转写(RTASR 标准版)参数 ============ */
#define RTASR_HOST               "rtasr.xfyun.cn"
#define RTASR_PORT               (80)         /*!< 80=ws, 443=wss */
#define RTASR_USE_TLS            (0)          /*!< 0=ws(不加密,固件小);1=wss */
#define RTASR_PATH               "/v1/ws"
#define RTASR_LANG               "cn"         /*!< 语种:cn=中文/中英混合 */
#define RTASR_VAD_MDN            (2)          /*!< 远近场:1=远场,2=近场 */
#define RTASR_PD                 ""           /*!< 垂直领域,空=通用 */
#define RTASR_CONNECT_TIMEOUT_MS (15000)      /*!< 连接+握手超时 */
#define RTASR_SEND_TIMEOUT_MS    (1000)       /*!< 发送超时 */

/* 识别结果回调(在 esp_websocket_client 事件回调上下文调用,勿做阻塞操作)
 * text: 本次识别文本; is_final: true=最终结果; user_ctx: 自定义参数 */
typedef void (*rtasr_result_cb_t)(const char *text, bool is_final, void *user_ctx);

/*!
 * 讯飞实时语音转写客户端驱动(基于官方 esp_websocket_client 组件,默认 ws:// 不加密)
 * 使用示例:
 *   RtAsr asr;
 *   asr.set_result_callback(cb, NULL);
 *   asr.init();                       // 内部先 SNTP 同步时间,再 ws 握手
 *   asr.send_audio(pcm, 1280, 1000);  // 每 40ms 发 1280 字节
 *   asr.finish(1000);                 // 发送结束标识
 */
class RtAsr {
public:
    esp_err_t init(void);                                   /*!< SNTP 同步时间→生成签名→ws 握手 */
    esp_err_t send_audio(const uint8_t *data, size_t len, uint32_t timeout_ms);  /*!< 发送二进制音频帧 */
    esp_err_t finish(uint32_t timeout_ms);                  /*!< 发送结束标识 {"end": true} */
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
