#include "drivers/llm.hpp"

#include <stdio.h>

#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "llm";

void Llm::attach(WS &ws)
{
    m_ws = &ws;
    /* 注册对话服务回复帧处理器 */
    ws.set_handler("reply", &Llm::handle_reply, this);
    ESP_LOGI(TAG, "Llm 已绑定 WS");
}

void Llm::set_reply_callback(llm_reply_cb_t cb, void *user_ctx)
{
    m_cb = cb;
    m_cb_ctx = user_ctx;
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
        ESP_LOGI(TAG, "回复: %s", content_item->valuestring);
        if (self->m_cb != NULL) {
            self->m_cb(content_item->valuestring, self->m_cb_ctx);
        }
    }
    cJSON_Delete(root);
}
