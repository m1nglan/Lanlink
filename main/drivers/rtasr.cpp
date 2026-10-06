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

/* ============ 三个 type 的公共前半段 ============
 * partial / final / revise 三个回调**只差"取到 text 之后干什么"**这一行,
 * 前面的 cJSON 解析样板完全一样 —— 抽到这里, 免得改一处漏两处。
 *
 * 成功返回 true, 并把 root 通过 root_out 交回调用方去 cJSON_Delete;
 * *text 指向 root 内部的内存, 所以调用方**必须先用 text、再 delete root**。 */
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

/* 语音增量: 服务器每条 partial 只含新增字,追加到累积 buffer 并流式显示 */
void RtAsr::handle_partial(const char *payload, int len, void *ctx)
{
    (void)len;
    RtAsr *self = static_cast<RtAsr *>(ctx);
    const char *text = NULL;
    cJSON *root = NULL;
    if (self != nullptr && extract_text(payload, &text, &root)) {
        self->accumulate(text, false);   /* is_final=false → 追加 */
        cJSON_Delete(root);
    }
}

/* 语音完成: 服务器发最终完整文本,覆盖 buffer 并触发完成回调 */
void RtAsr::handle_final(const char *payload, int len, void *ctx)
{
    (void)len;
    RtAsr *self = static_cast<RtAsr *>(ctx);
    const char *text = NULL;
    cJSON *root = NULL;
    if (self != nullptr && extract_text(payload, &text, &root)) {
        self->accumulate(text, true);    /* is_final=true → 覆盖 */
        cJSON_Delete(root);
    }
}

/* 语音修正: 服务器发完整当前文本,整体替换 buffer(修正错字),**不触发完成回调**
 * ★ 别把它并进 handle_final: revise 故意不发 m_cb, 否则 UI 会收到假的"说完了" */
void RtAsr::handle_revise(const char *payload, int len, void *ctx)
{
    (void)len;
    RtAsr *self = static_cast<RtAsr *>(ctx);
    const char *text = NULL;
    cJSON *root = NULL;
    if (self != nullptr && extract_text(payload, &text, &root)) {
        self->revise(text);
        cJSON_Delete(root);
    }
}

/* 语音修正核心: 整体替换 buffer, 不触发完成回调 */
void RtAsr::revise(const char *text)
{
    strlcpy(s_result, text, sizeof(s_result));
    ESP_LOGD(TAG, "[修正] %s", s_result);
}

/* 累积核心: 增量追加(partial)或完整覆盖(final)。
 * is_final=false: strlcat 追加; is_final=true: strlcpy 覆盖, 并触发 m_cb
 *
 * ★★ 本函数(以及 revise)是**在 WS 回调里**跑的, 而 WS 回调由 websocket_task
 *   在**持有 esp_websocket_client 的 client->lock** 的情况下调用。
 *   所以这里**绝对不能有耗时/阻塞操作**。
 *
 *   原实现是 printf + fflush(stdout), 有两个后果(实测都踩到了):
 *     ① 持 client->lock 打串口 → 拖住 voice_task 的 send_audio
 *     ② 往 UART 灌大量字符 → UART 中断频繁触发 (那次 Interrupt WDT panic 的
 *        EPC1 正是 uart_hal_write_txfifo), 在 CPU0 上和 GPIO 中断一起
 *        把中断看门狗饿死
 *   而且**完全没必要**: 文字已经通过 m_cb 交给 UI 了, 再往串口打一遍是纯浪费。
 *   故降级为 ESP_LOGD (默认不输出; 要看就 esp_log_level_set("rtasr", ESP_LOG_DEBUG)) */
void RtAsr::accumulate(const char *text, bool is_final)  //< 拼接回复: 把增量文字拼在原有文字上
{
    if (is_final) {
        strlcpy(s_result, text, sizeof(s_result));
        ESP_LOGD(TAG, "[识别] %s", s_result);      /* 完整结果 */
    } else {
        strlcat(s_result, text, sizeof(s_result));
        ESP_LOGD(TAG, "%s", text);                 /* 流式增量 */
    }

    if (m_cb != NULL) {
        m_cb(s_result, is_final, m_cb_ctx);   /* 交给调度层(现在是 UiBridge → 投队列) */
    }
}
