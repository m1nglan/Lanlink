#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#include "drivers/ws.hpp"

/* 对话回复回调(在 WS 消息分发上下文调用,勿做阻塞操作)
 * text: 模型回复内容; user_ctx: 自定义参数 */
typedef void (*llm_reply_cb_t)(const char *text, void *user_ctx);

/*!
 * 大模型对话(llm / openclaw)业务驱动,基于共享 WS 连接
 * 用 switch_service() 参数切换两个服务:
 *   - "llm":      DeepSeek,网关维护上下文
 *   - "openclaw": 转发明岚对话
 * 使用示例:
 *   WS::get().init();
 *   Llm llm;
 *   llm.attach(WS::get());
 *   llm.set_reply_callback(cb, NULL);
 *   llm.switch_service("llm", 1000);      // 切到 DeepSeek
 *   llm.chat("记作业:数学第3页", 1000);     // 发对话
 *   llm.clear(1000);                       // 清空上下文
 */
class Llm {
public:
    void attach(WS &ws);                       /*!< 绑定共享连接,注册 partial/reply 处理器 */
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
    llm_reply_cb_t m_cb = NULL;
    void *m_cb_ctx = NULL;
};
