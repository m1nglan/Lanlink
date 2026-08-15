#include "drivers/rtasr.hpp"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "rtasr";

/* 累积识别文字 buffer: partial 追加, final 覆盖, 每轮 start 时清零 */
static char s_result[1024];

void RtAsr::attach(WS &ws)
{
    m_ws = &ws;
    /* 注册 text 服务的结果帧处理器 */
    ws.set_handler("partial", &RtAsr::handle_partial, this);
    ws.set_handler("final", &RtAsr::handle_final, this);
    ESP_LOGI(TAG, "RtAsr 已绑定 WS");
}

void RtAsr::set_result_callback(rtasr_result_cb_t cb, void *user_ctx)
{
    m_cb = cb;
    m_cb_ctx = user_ctx;
}

esp_err_t RtAsr::switch_service(const char *service, uint32_t timeout_ms)
{
    if (m_ws == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "{\"type\":\"svc\",\"service\":\"%s\"}", service);
    ESP_LOGI(TAG, "切换服务: %s", service);
    return m_ws->send_text(msg, timeout_ms);
}

esp_err_t RtAsr::start(uint32_t timeout_ms)
{
    if (m_ws == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = m_ws->send_text("{\"type\":\"start\"}", timeout_ms);
    if (ret == ESP_OK) {
        s_result[0] = '\0';  /* 新一轮开始,清空累积文字 */
    }
    return ret;
}

esp_err_t RtAsr::send_audio(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (m_ws == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return m_ws->send_bin(data, len, timeout_ms);
}

esp_err_t RtAsr::end(uint32_t timeout_ms)
{
    if (m_ws == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return m_ws->send_text("{\"type\":\"end\"}", timeout_ms);
}

void RtAsr::handle_partial(const char *payload, int len, void *ctx)
{
    RtAsr *self = static_cast<RtAsr *>(ctx);
    if (self == nullptr) {
        return;
    }

    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        return;
    }
    const cJSON *text_item = cJSON_GetObjectItem(root, "text");
    if (text_item != NULL && cJSON_IsString(text_item) && text_item->valuestring[0] != '\0') {
        self->accumulate(text_item->valuestring, false);
    }
    cJSON_Delete(root);
}

void RtAsr::handle_final(const char *payload, int len, void *ctx)
{
    RtAsr *self = static_cast<RtAsr *>(ctx);
    if (self == nullptr) {
        return;
    }

    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        return;
    }
    const cJSON *text_item = cJSON_GetObjectItem(root, "text");
    if (text_item != NULL && cJSON_IsString(text_item) && text_item->valuestring[0] != '\0') {
        self->accumulate(text_item->valuestring, true);
    }
    cJSON_Delete(root);
}

void RtAsr::accumulate(const char *text, bool is_final)
{
    /* 服务器增量返回: partial 只回新字符,追加到累积 buffer; final 回完整文本,覆盖 */
    if (is_final) {
        strlcpy(s_result, text, sizeof(s_result));
    } else {
        strlcat(s_result, text, sizeof(s_result));
    }

    /* 中间结果同一行实时刷新(\r 回到行首覆盖),最终结果换行 */
    if (is_final) {
        printf("\r识别结果: %s\n", s_result);
    } else {
        printf("\r识别中: %s    ", s_result);
    }
    fflush(stdout);

    if (m_cb != NULL) {
        m_cb(s_result, is_final, m_cb_ctx);
    }
}
