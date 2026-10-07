#include "drivers/llm.hpp"

#include <stdio.h>

#include "esp_log.h"
#include "esp_attr.h"
#include "cJSON.h"

static const char *TAG = "llm";

/* ================================================================
 * [链] websocket_task → WS::dispatch_msg → 【handle_partial / handle_reply】
 *      → [回调] m_stream_cb → UiBridge → post_stream → stream_q → 气泡
 *
 * ★★ 累积【在这里闸口做完】: 每条 partial 同步并进 s_stream, 交出去的是完整回复。
 *    放到 UI 侧会丢字 —— stream_q 深度 1 覆盖式, 详见 llm.hpp / MD/HANDOFF.md §6-F。
 * V2 协议详见 MD/PROTOCOL.md。
 * ================================================================ */

/* 对话流式累积 buffer: partial(流式字)追加, reply(完整)覆盖。
 * EXT_RAM_BSS_ATTR: 放 PSRAM 省内部 SRAM(文字低频访问,不影响速度) */
EXT_RAM_BSS_ATTR static char s_stream[2048];

void Llm::attach(WS &ws)
{
    m_ws = &ws;
    /* V2: partial 只属于 llm/openclaw (text 服务改用 "asr"),
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

/* 对话流式增量 ({"type":"partial"})。 [链同]
 * ★★ 累积在这里做完 —— 交出去的已经是【完整回复】, 不是这一小段增量。
 *   (V2 相对 V1 的核心改动, 别退回去。) */
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
