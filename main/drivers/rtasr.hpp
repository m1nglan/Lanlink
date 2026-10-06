#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "drivers/ws.hpp"

/* ================================================================
 * RtAsr: 语音听写(text 服务)业务驱动   —— V2 协议 (见 PROTOCOL.md)
 *
 * 网关下发的流式消息是 **{"type":"asr","text":"<完整当前句>"}** —— 每次都是
 * 从头到现在的完整句, **不是增量**。所以本驱动收到就直接【整体替换】,
 * 不存在任何累积状态。
 *
 * 消息类型:
 *   asr    → handle_asr  (完整当前句, 覆盖)
 *   final  → handle_final(完整最终句, 覆盖 + 触发完成回调)
 *
 * ★ V1 的 "partial" 和 "revise" 已废除:
 *   - partial 现在只属于 llm/openclaw 服务
 *   - revise  因为 asr 每次都是全量(自带修正), 不再需要
 * ================================================================ */

/* 识别结果回调(在 WS 消息分发上下文调用, 勿做阻塞操作)
 * text:     **完整**识别文本 (V2: 每次都是全量, 调用方无需累积)
 * is_final: true = 最终结果(本轮完成)
 * user_ctx: 自定义参数 */
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
