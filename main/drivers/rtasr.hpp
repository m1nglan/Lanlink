#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "drivers/ws.hpp"

/* 识别结果回调(在 WS 消息分发上下文调用,勿做阻塞操作)
 * text: 累积识别文本; is_final: true=最终结果; user_ctx: 自定义参数 */
typedef void (*rtasr_result_cb_t)(const char *text, bool is_final, void *user_ctx);

/*!
 * 语音听写(text 服务)业务驱动,基于共享 WS 连接
 * 使用示例:
 *   WS::get().init();
 *   RtAsr asr;
 *   asr.attach(WS::get());
 *   asr.set_result_callback(cb, NULL);
 *   asr.start(1000);               // 发 {"type":"start"}
 *   asr.send_audio(pcm, 1280, 1000); // 发音频
 *   asr.end(1000);                 // 发 {"type":"end"}
 */
class RtAsr {
public:
    void attach(WS &ws);                       /*!< 绑定共享连接,并注册 partial/final 处理器 */
    void set_result_callback(rtasr_result_cb_t cb, void *user_ctx);

    esp_err_t switch_service(const char *service, uint32_t timeout_ms);  /*!< 切到指定服务: "text"/"llm"/"openclaw" */
    esp_err_t start(uint32_t timeout_ms);      /*!< 发 {"type":"start"},清空累积 */
    esp_err_t send_audio(const uint8_t *data, size_t len, uint32_t timeout_ms);  /*!< 发音频帧 */
    esp_err_t end(uint32_t timeout_ms);        /*!< 发 {"type":"end"} */

private:
    /* 由 WS 按 type 分发的处理器(静态,签名 ws_msg_handler_t) */
    static void handle_partial(const char *payload, int len, void *ctx);
    static void handle_final(const char *payload, int len, void *ctx);
    static void handle_revise(const char *payload, int len, void *ctx);  /* 语音修正 */
    void accumulate(const char *text, bool is_final);
    void revise(const char *text);   /* 整体替换 buffer 并重打印(修正) */

    WS *m_ws = nullptr;
    rtasr_result_cb_t m_cb = NULL;
    void *m_cb_ctx = NULL;
};
