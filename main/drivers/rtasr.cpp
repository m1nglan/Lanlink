#include "drivers/rtasr.hpp"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_attr.h"
#include "cJSON.h"

static const char *TAG = "rtasr";

/* ================================================================
 * RtAsr: 语音听写(text 服务)业务驱动
 * 依赖共享 WS 连接,通过 set_handler 注册消息处理器,由 WS 按 type 分发:
 *   partial(语音增量) → handle_partial(追加 buffer,流式显示)
 *   revise(语音修正)  → handle_revise(整体替换 buffer,修正错字)
 *   final(语音完成)   → handle_final(覆盖 buffer + 触发完成回调)
 * ================================================================ */

/* 累积识别文字 buffer: partial 追加, revise/final 覆盖, 每轮 start 时清零。
 * EXT_RAM_BSS_ATTR: 放 PSRAM 省内部 SRAM(文字低频访问,不影响速度) */
EXT_RAM_BSS_ATTR static char s_result[1024];

void RtAsr::attach(WS &ws)
{
    m_ws = &ws;
    /* 注册 text 服务的处理器:
     *   partial → set_partial_handler("text"), 由 WS 按当前服务路由
     *   final/revise → 普通 set_handler, 按 type 精确匹配 */
    ws.set_partial_handler("text", &RtAsr::handle_partial, this);
    ws.set_handler("final", &RtAsr::handle_final, this);
    ws.set_handler("revise", &RtAsr::handle_revise, this);
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
    return m_ws->send_bin(data, len, timeout_ms);   /* 音频走二进制帧 */
}

esp_err_t RtAsr::end(uint32_t timeout_ms)
{
    if (m_ws == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return m_ws->send_text("{\"type\":\"end\"}", timeout_ms);
}

/* 语音增量: 服务器每条 partial 只含新增字,追加到累积 buffer 并流式显示 */
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
        self->accumulate(text_item->valuestring, false);   /* is_final=false → 追加 */
    }
    cJSON_Delete(root);
}

/* 语音修正: 服务器发完整当前文本,整体替换 buffer(修正错字),不回 IDLE/不触发完成 */
void RtAsr::handle_revise(const char *payload, int len, void *ctx)
{
    (void)len;
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
        self->revise(text_item->valuestring);   /* 修正: 替换 buffer,不触发完成 */
    }
    cJSON_Delete(root);
}

/* 语音完成: 服务器发最终完整文本,覆盖 buffer 并触发完成回调(回 IDLE + LLM 转发) */
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
        self->accumulate(text_item->valuestring, true);   /* is_final=true → 覆盖 */
    }
    cJSON_Delete(root);
}

/* 语音修正核心: 整体替换 buffer,换行重打整条(标记[修正]),不触发完成回调 */
void RtAsr::revise(const char *text)
{
    strlcpy(s_result, text, sizeof(s_result));
    printf("\n[修正] %s", s_result);   /* 换行 + [修正] 前缀,直观看到修正处 */
    fflush(stdout);
}

/* 累积核心: 增量追加(partial)或完整覆盖(final),并打印显示
 * is_final=false: strlcat 追加, 打印新增字(流式)
 * is_final=true:  strlcpy 覆盖, 打印完整结果(换行定格), 并触发 m_cb 完成回调 */
void RtAsr::accumulate(const char *text, bool is_final)
{
    if (is_final) {
        strlcpy(s_result, text, sizeof(s_result));
        printf("\n[识别] %s\n", s_result);   /* 完整结果: 换行定格 */
    } else {
        strlcat(s_result, text, sizeof(s_result));
        printf("%s", text);   /* 流式: 只打印新增字符,不换行,字从行尾冒出来 */
    }
    fflush(stdout);

    if (m_cb != NULL) {
        m_cb(s_result, is_final, m_cb_ctx);   /* final 时触发完成回调(回 IDLE + LLM 转发) */
    }
}
