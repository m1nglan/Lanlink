#include "drivers/rtasr.hpp"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_attr.h"
#include "cJSON.h"

static const char *TAG = "rtasr";

/* ================================================================
 * [链] websocket_task → WS::handle_data → WS::dispatch_msg
 *      → 【handle_asr / handle_final】 → store_and_notify
 *        → [回调] UiBridge::on_asr_result → post_stream → stream_q → 气泡
 *
 * 注册 "asr"/"final" 两个 type, 都由 WS 查表分发(不再有"按服务分流")。
 * V2 协议细节详见 MD/PROTOCOL.md。
 * ================================================================ */

/* 识别文字 buffer —— V2 下它只是"最近一条完整句"的存放处, 不做累积。
 * EXT_RAM_BSS_ATTR: 放 PSRAM 省内部 SRAM (文字低频访问, 不影响速度) */
EXT_RAM_BSS_ATTR static char s_result[1024];

void RtAsr::attach(WS &ws)
{
    m_ws = &ws;
    /* 注册 text 服务的处理器 (V2: 都是普通 set_handler, 按 type 精确匹配) */
    ws.set_handler("asr",   &RtAsr::handle_asr,   this);
    ws.set_handler("final", &RtAsr::handle_final, this);
    ESP_LOGI(TAG, "RtAsr 已绑定 WS (V2: asr/final)");
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

/* asr / final 两个回调共用的前半段: 解析 JSON + 取 "text" 字段。
 * 成功返回 true 并把 root 交回调用方 delete; *text 指向 root 内部内存
 * → 调用方**必须先用 text、再 delete root**。 */
static bool extract_text(const char *payload, const char **text, cJSON **root_out)
{
    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        return false;
    }
    const cJSON *text_item = cJSON_GetObjectItem(root, "text");
    if (text_item == NULL || !cJSON_IsString(text_item) || text_item->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return false;
    }
    *text     = text_item->valuestring;
    *root_out = root;
    return true;
}

/* 流式识别 ({"type":"asr"}): 网关发的是【完整当前句】, 整体替换后通知 UI */
void RtAsr::handle_asr(const char *payload, int len, void *ctx)
{
    (void)len;
    RtAsr *self = static_cast<RtAsr *>(ctx);
    const char *text = NULL;
    cJSON *root = NULL;
    if (self != nullptr && extract_text(payload, &text, &root)) {
        self->store_and_notify(text, false);   /* false = 还没说完 */
        cJSON_Delete(root);
    }
}

/* 语音完成 ({"type":"final"}): 网关发【完整最终句】, 整体替换后通知 UI + 标记完成 */
void RtAsr::handle_final(const char *payload, int len, void *ctx)
{
    (void)len;
    RtAsr *self = static_cast<RtAsr *>(ctx);
    const char *text = NULL;
    cJSON *root = NULL;
    if (self != nullptr && extract_text(payload, &text, &root)) {
        self->store_and_notify(text, true);    /* true = 本轮完成 */
        cJSON_Delete(root);
    }
}

/* 核心: 整体替换 buffer + 触发回调。 [链同]
 *
 * ★★ V2: 这里【没有累积】—— 网关每次给的都是完整句, 直接 strlcpy 覆盖。
 * ★★ 本函数在 WS 回调里跑, 而回调由 websocket_task **持着 client->lock** 调用
 *   → 绝对不能有耗时/阻塞操作。原版有 printf+fflush: 会拖住 send_audio, 并把
 *   UART 中断打爆(Interrupt WDT panic)。详见 MD/HANDOFF.md §6-I */
void RtAsr::store_and_notify(const char *text, bool is_final)
{
    strlcpy(s_result, text, sizeof(s_result));   /* ★ 整体替换, 不追加 */
    ESP_LOGD(TAG, "[%s] %s", is_final ? "final" : "asr", s_result);

    if (m_cb != NULL) {
        m_cb(s_result, is_final, m_cb_ctx);   /* 交给调度层(UiBridge → 投 stream_q) */
    }
}
