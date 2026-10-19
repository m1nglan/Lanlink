#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "drivers/ws.hpp"

/* ================================================================
 * RtAsr: 语音听写(text 服务)驱动  —— V2 协议, 详见 MD/PROTOCOL.md
 *
 * [链] websocket_task → WS::dispatch_msg → 【handle_asr / handle_final】
 *      → store_and_notify → [回调] UiBridge::on_asr_result → stream_q → 气泡
 *
 * 网关每次下发都是【完整句】("asr" / "final"), **不是增量** → 本驱动【零累积】,
 * 每次 strlcpy 整体替换。好处: 丢一条 / 乱序 / 重复 都不影响最终结果。
 * (V1 的 partial / revise 已废除: partial 归 llm, revise 因 asr 自带修正而多余。)
 * ================================================================ */

/* 识别结果回调(在 WS 消息分发上下文调用, 勿做阻塞操作)
 * text:     **完整**识别文本 (V2: 每次都是全量, 调用方无需累积)
 * is_final: true = 最终结果(本轮完成)
 * user_ctx: 自定义参数 */
typedef void (*rtasr_result_cb_t)(const char *text, bool is_final, void *user_ctx);

/*! 语音听写(text 服务)驱动, 基于共享 WS 连接。
 *  用法: asr.attach(WS::get());  asr.set_result_callback(cb, NULL);
 *        asr.start(1000);  asr.send_audio(pcm, 1280, 1000);  asr.end(1000); */
class RtAsr {
public:
    void attach(WS &ws);                       /*!< 绑定共享连接, 注册 asr/final 处理器 */
    void set_result_callback(rtasr_result_cb_t cb, void *user_ctx);

    esp_err_t switch_service(const char *service, uint32_t timeout_ms);  /*!< 切到指定服务: "text"/"llm"/"openclaw" */
    esp_err_t start(uint32_t timeout_ms);      /*!< 发 {"type":"start"}, 清空 buffer */
    esp_err_t send_audio(const uint8_t *data, size_t len, uint32_t timeout_ms);  /*!< 发音频帧 */
    esp_err_t end(uint32_t timeout_ms);        /*!< 发 {"type":"end"} */

private:
    /* 由 WS 按 type 分发的处理器(静态, 签名 ws_msg_handler_t) */
    static void handle_asr(const char *payload, int len, void *ctx);     /* {"type":"asr"} */
    static void handle_final(const char *payload, int len, void *ctx);   /* {"type":"final"} */
    void store_and_notify(const char *text, bool is_final);              /* 整体替换 + 回调 */

    WS *m_ws = nullptr;
    rtasr_result_cb_t m_cb = NULL;
    void *m_cb_ctx = NULL;
};
