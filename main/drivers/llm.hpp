#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#include "drivers/ws.hpp"

/* 对话回复回调(在 WS 消息分发上下文调用,勿做阻塞操作)
 * text: 模型回复内容; user_ctx: 自定义参数 */
typedef void (*llm_reply_cb_t)(const char *text, void *user_ctx);

/* 流式文本回调 —— **与 rtasr_result_cb_t 同形状**, 这样 UiBridge 能用同一套代码接两者。
 * ★ text 永远是【完整回复】(累积已在驱动侧做完), 不是增量。
 *   is_final = true 表示本轮完成 (收到 reply)。 */
typedef void (*llm_stream_cb_t)(const char *text, bool is_final, void *user_ctx);

/* ================================================================
 * Llm: 大模型对话(llm / openclaw)业务驱动  —— V2 协议 (见 PROTOCOL.md)
 *
 * 用 switch_service() 参数切换两个服务:
 *   - "llm":      DeepSeek, 网关维护上下文
 *   - "openclaw": 转发明岚对话
 *
 * ★ V2: 网关对 LLM 仍发**增量**({"type":"partial"}), 但
 *   **累积在驱动侧做完**, 交给 set_stream_callback 的永远是【完整回复】。
 *   理由见 llm.cpp 顶部注释 (核心: 总线上的 stream_q 是覆盖式, 放增量会丢字)。
 *
 * 使用示例:
 *   WS::get().init();
 *   Llm llm;
 *   llm.attach(WS::get());
 *   llm.set_stream_callback(cb, NULL);    // cb(text, is_final, ctx)
 *   llm.switch_service("llm", 1000);      // 切到 DeepSeek
 *   llm.chat("记作业:数学第3页", 1000);     // 发对话
 *   llm.clear(1000);                       // 清空上下文
 * ================================================================ */
class Llm {
public:
    void attach(WS &ws);                       /*!< 绑定共享连接, 注册 partial/reply 处理器 */

    /*! 注册流式回调 (V2 推荐用这个): text 永远是【完整回复】, is_final 标记本轮完成 */
    void set_stream_callback(llm_stream_cb_t cb, void *user_ctx);
    /*! 兼容旧接口: 只在收到 reply(完整回复)时触发 */
    void set_reply_callback(llm_reply_cb_t cb, void *user_ctx);

    /*! 切换服务: "llm" 或 "openclaw",发 {"type":"svc","service":...} */
    esp_err_t switch_service(const char *service, uint32_t timeout_ms);
    /*! 发对话 {"type":"chat","content":...} */
    esp_err_t chat(const char *content, uint32_t timeout_ms);
    /*! 清空上下文 {"type":"clear"} */
    esp_err_t clear(uint32_t timeout_ms);
    /*! 清空流式累积 buffer(chat 前调用) */
    void reset_stream(void);

private:
    /* 由 WS 分发的处理器(静态,签名 ws_msg_handler_t) */
    static void handle_partial(const char *payload, int len, void *ctx);  /* 对话流式增量 */
    static void handle_reply(const char *payload, int len, void *ctx);    /* 完整回复 */

    WS *m_ws = nullptr;
    llm_stream_cb_t m_stream_cb  = NULL;   /*!< V2: 收增量时也带完整文本回调 */
    void           *m_stream_ctx = NULL;
    llm_reply_cb_t m_cb = NULL;
    void *m_cb_ctx = NULL;
};
