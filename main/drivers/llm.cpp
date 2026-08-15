#include "drivers/llm.hpp"

#include <stdio.h>

#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "llm";

/* ================================================================
 * Llm: 大模型对话(llm/openclaw 服务)业务驱动
 * 依赖共享 WS 连接,通过 set_handler 注册消息处理器,由 WS 按 type 分发:
 *   partial(对话流式增量) → handle_partial(追加 buffer,逐字流式显示)
 *   reply(完整回复)       → handle_reply(覆盖 buffer,换行定格,置完成标志)
 * 注: partial 与语音识别的 partial 同名,由 WS 按当前服务路由(RtAsr↔Llm)。
 * ================================================================ */

/* 对话流式累积 buffer: partial(流式字)追加, reply(完整)覆盖 */
static char s_stream[1024];

void Llm::attach(WS &ws)
{
    m_ws = &ws;
    /* 注册对话服务的处理器:
     *   partial → set_partial_handler("llm"), 由 WS 按当前服务路由
     *   reply → 普通 set_handler, 按 type 精确匹配 */
    ws.set_partial_handler("llm", &Llm::handle_partial, this);
    ws.set_handler("reply", &Llm::handle_reply, this);
    ESP_LOGI(TAG, "Llm 已绑定 WS");
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

/* 对话流式增量: 服务器每条 partial 只含新增字,追加到累积 buffer 并逐字打印
 * (不换行,字从行尾冒出来 → 流式效果; 任何终端都正常,不依赖 ANSI/换行) */
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
        strlcat(s_stream, text_item->valuestring, sizeof(s_stream));   /* 累积 */
        printf("%s", text_item->valuestring);   /* 只打印新增字,不换行 */
        fflush(stdout);
    }
    cJSON_Delete(root);
}

/* 完整回复: 覆盖流式累积,收到即本轮完成(换行定格),并触发完成回调 */
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
        strlcpy(s_stream, content_item->valuestring, sizeof(s_stream));   /* 覆盖流式累积 */
        printf("\n[OpenClaw] %s\n", s_stream);   /* 完整回复: 换行定格 */
        fflush(stdout);
        if (self->m_cb != NULL) {
            self->m_cb(s_stream, self->m_cb_ctx);   /* 触发 on_llm_reply(置 s_reply_received) */
        }
    }
    cJSON_Delete(root);
}
