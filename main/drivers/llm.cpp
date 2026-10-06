#include "drivers/llm.hpp"

#include <stdio.h>

#include "esp_log.h"
#include "esp_attr.h"
#include "cJSON.h"

static const char *TAG = "llm";

/* ================================================================
 * Llm: 大模型对话(llm/openclaw 服务)业务驱动  —— V2 协议 (见 PROTOCOL.md)
 * 依赖共享 WS 连接, 通过 set_handler 注册消息处理器, 由 WS 按 type 分发:
 *   partial(流式增量) → handle_partial(累积到 buffer, 再把【完整回复】投给总线)
 *   reply  (完整回复) → handle_reply  (覆盖 buffer + 置完成标志)
 *
 * ★★ V2 关键: 网关对 LLM 仍然发**增量**(每字一条, 量大), 但账要算在驱动侧 ——
 *   **累积在这里(WS 回调里)同步做完, 送给总线的永远是【完整回复】**。
 *
 *   为什么不能在 UI 侧累积: 总线上的 stream_q 是【深度 1 覆盖式】,
 *   如果增量原样放进去, UI 每 ~137ms 才取一次 → 中间的增量被覆盖 → 永久丢失。
 *   在驱动侧累积则每条 partial 都同步并进 s_stream, 覆盖多少次都无所谓。
 *
 *   这与 RtAsr 的差别只是"谁做累积"(ASR 是网关做, LLM 是板子做),
 *   但【总线上永远只有全量】这条规则是一致的 —— 所以 UI 侧零状态。
 * ================================================================ */

/* 对话流式累积 buffer: partial(流式字)追加, reply(完整)覆盖。
 * EXT_RAM_BSS_ATTR: 放 PSRAM 省内部 SRAM(文字低频访问,不影响速度) */
EXT_RAM_BSS_ATTR static char s_stream[2048];

void Llm::attach(WS &ws)
{
    m_ws = &ws;
    /* V2: partial 只属于 llm/openclaw 服务 (text 服务改用 "asr"),
     * 所以它和 reply 一样走普通 set_handler, 不需要"按服务分流"。 */
    ws.set_handler("partial", &Llm::handle_partial, this);
    ws.set_handler("reply",   &Llm::handle_reply,   this);
    ESP_LOGI(TAG, "Llm 已绑定 WS (V2: partial/reply)");
}

void Llm::set_stream_callback(llm_stream_cb_t cb, void *user_ctx)
{
    m_stream_cb  = cb;
    m_stream_ctx = user_ctx;
}

void Llm::set_reply_callback(llm_reply_cb_t cb, void *user_ctx)
{
    m_cb = cb;
    m_cb_ctx = user_ctx;
}

void Llm::reset_stream(void)
{
    s_stream[0] = '\0';   /* 每轮 chat 前清空流式累积 */
}

esp_err_t Llm::switch_service(const char *service, uint32_t timeout_ms)
{
    if (m_ws == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "{\"type\":\"svc\",\"service\":\"%s\"}", service);
    ESP_LOGI(TAG, "切换服务: %s", service);
    return m_ws->send_text(msg, timeout_ms);
}

esp_err_t Llm::chat(const char *content, uint32_t timeout_ms)
{
    if (m_ws == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 用 cJSON 构造,避免 content 含引号/特殊字符破坏 JSON */
    char msg[1024];
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "chat");
    cJSON_AddStringToObject(root, "content", content);
    int ok = cJSON_PrintPreallocated(root, msg, sizeof(msg), 0);
    cJSON_Delete(root);
    if (!ok) {
        ESP_LOGE(TAG, "chat 消息过长,放不下 buffer");
        return ESP_ERR_INVALID_SIZE;
    }
    return m_ws->send_text(msg, timeout_ms);
}

esp_err_t Llm::clear(uint32_t timeout_ms)
{
    if (m_ws == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return m_ws->send_text("{\"type\":\"clear\"}", timeout_ms);
}

/* 对话流式增量 ({"type":"partial"})。
 * ★★ V2: 累积【在这里】做完 —— 每条 partial 都同步并进 s_stream,
 *   然后把【完整回复】交给上层 (→ stream_q)。这样总线消费者零累积状态,
 *   而且 stream_q 是覆盖式, 无论被覆盖多少次都不会丢字。 */
void Llm::handle_partial(const char *payload, int len, void *ctx)
{
    (void)len;
    Llm *self = static_cast<Llm *>(ctx);
    if (self == nullptr) {
        return;
    }

    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        return;
    }
    const cJSON *text_item = cJSON_GetObjectItem(root, "text");
    if (text_item != NULL && cJSON_IsString(text_item) && text_item->valuestring[0] != '\0') {
        strlcat(s_stream, text_item->valuestring, sizeof(s_stream));   /* 累积增量 */

        /* ★ 关键: 交出去的已经是【完整回复】, 不是这一小段增量。
         *   (这条注释就是 V2 相对 V1 的核心改动, 别退回去。) */
        if (self->m_stream_cb != NULL) {
            self->m_stream_cb(s_stream, false, self->m_stream_ctx);
        }
    }
    cJSON_Delete(root);
}

/* 完整回复 ({"type":"reply"}): 覆盖流式累积, 收到即本轮完成, 触发完成回调 */
void Llm::handle_reply(const char *payload, int len, void *ctx)
{
    (void)len;
    Llm *self = static_cast<Llm *>(ctx);
    if (self == nullptr) {
        return;
    }

    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        return;
    }
    const cJSON *content_item = cJSON_GetObjectItem(root, "content");
    if (content_item != NULL && cJSON_IsString(content_item) && content_item->valuestring[0] != '\0') {
        strlcpy(s_stream, content_item->valuestring, sizeof(s_stream));   /* 覆盖累积 */

        /* 先按"完整文本"投一条 is_final=true —— 这样 UI 只靠 stream_q 就能
         * 同时拿到文本和"完成"标志, 不必再走 resp_q。 */
        if (self->m_stream_cb != NULL) {
            self->m_stream_cb(s_stream, true, self->m_stream_ctx);
        }
        if (self->m_cb != NULL) {
            self->m_cb(s_stream, self->m_cb_ctx);   /* 兼容原有的"完成回调" */
        }
    }
    cJSON_Delete(root);
}
