#include "business/ui_bridge.hpp"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "drivers/ws.hpp"

#include "lvgl.h"

static const char *TAG = "ui_bridge";

/*! UI 消费周期。partial 再高频, 也只在"队列非空(有新内容)"时才刷。 */
#define UI_TIMER_PERIOD_MS   (50)

UiBridge& UiBridge::get(void)
{
    static UiBridge s_inst;   /* C++11 起局部 static 初始化线程安全 */
    return s_inst;
}

esp_err_t UiBridge::init(void)
{
    if (m_cmd_q != nullptr) {
        return ESP_ERR_INVALID_STATE;   /* 重复 init */
    }

    m_cmd_q    = xQueueCreate(CMD_Q_LEN,    sizeof(cmd_msg_t));
    m_resp_q   = xQueueCreate(RESP_Q_LEN,   sizeof(resp_msg_t));
    m_stream_q = xQueueCreate(STREAM_Q_LEN, sizeof(stream_msg_t));
    if (m_cmd_q == nullptr || m_resp_q == nullptr || m_stream_q == nullptr) {
        ESP_LOGE(TAG, "队列创建失败 (cmd=%p resp=%p stream=%p)",
                 m_cmd_q, m_resp_q, m_stream_q);
        return ESP_ERR_NO_MEM;
    }

    /* asr 绑到 WS 单例 + 注册结果回调。
     * attach 只做"往 WS 的 handler 表登记", 跟"有没有建连"无关 → 这里做一次就够。
     * ★ 重连**不需要**重新 attach: WS::deinit() 不清 handler 表,
     *   且 set_handler() 按 type 自带去重(重复注册不会撑满表)。 */
    m_asr.attach(WS::get());
    m_asr.set_result_callback(&UiBridge::on_asr_result, this);

    ESP_LOGI(TAG, "就绪: cmd_q=%d resp_q=%d stream_q=%d (共约 %u 字节内部 SRAM)",
             CMD_Q_LEN, RESP_Q_LEN, STREAM_Q_LEN,
             (unsigned)(CMD_Q_LEN * sizeof(cmd_msg_t)
                        + RESP_Q_LEN * sizeof(resp_msg_t)
                        + STREAM_Q_LEN * sizeof(stream_msg_t)));
    return ESP_OK;
}

/* ======================= 投递 (全部非阻塞) ======================= */

bool UiBridge::push_cmd(cmd_t cmd, int32_t arg)
{
    if (m_cmd_q == nullptr) {
        return false;
    }
    cmd_msg_t m = {};
    m.cmd = cmd;
    m.arg = arg;
    return xQueueSend(m_cmd_q, &m, 0) == pdTRUE;   /* timeout=0: 绝不阻塞 */
}

bool UiBridge::voice_start(void)
{
    bool ok = push_cmd(CMD_VOICE_START, 0);
    if (!ok) {
        ESP_LOGW(TAG, "cmd_q 满, CMD_VOICE_START 丢弃");
    }
    return ok;
}

bool UiBridge::voice_stop(void)
{
    bool ok = push_cmd(CMD_VOICE_STOP, 0);
    if (!ok) {
        ESP_LOGW(TAG, "cmd_q 满, CMD_VOICE_STOP 丢弃");
    }
    return ok;
}

bool UiBridge::svc_switch_pending(void)
{
    return push_cmd(CMD_SVC_SWITCH, 0);   /* 本阶段无人消费, 只占位 */
}

void UiBridge::post_resp(resp_kind_t kind, const char *text)
{
    if (m_resp_q == nullptr) {
        return;
    }
    resp_msg_t m = {};
    m.kind = kind;
    m.t_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (text != nullptr) {
        strlcpy(m.text, text, sizeof(m.text));   /* ★ 绝不用 strcpy: 防截断越界 */
    }

    /* ★ timeout=0 是硬要求: 本函数可能在 WS 回调里被调用,
     *   那时正持着 esp_websocket_client 的 client->lock。
     *   若这里阻塞, 整个 WS 收发(含 voice_task 发音频)都会被拖死。 */
    if (xQueueSend(m_resp_q, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "resp_q 满, 丢弃 kind=%d", (int)kind);
    }
}

void UiBridge::post_stream(stream_kind_t kind, const char *text)
{
    if (m_stream_q == nullptr) {
        return;
    }
    stream_msg_t m = {};
    m.kind = kind;
    if (text != nullptr) {
        strlcpy(m.text, text, sizeof(m.text));
    }
    /* xQueueOverwrite: 深度 1 覆盖式 → 永不阻塞、永不失败, 只留最新那句 */
    xQueueOverwrite(m_stream_q, &m);
}

/* ======================= 接收 (LVGL 侧) ======================= */

/* asr 结果回调: 跑在 websocket_task, 且持着 client->lock → 只做两件事 */
void UiBridge::on_asr_result(const char *text, bool is_final, void *ctx)
{
    UiBridge *self = static_cast<UiBridge *>(ctx);
    if (self == nullptr || text == nullptr) {
        return;
    }

    if (is_final) {
        self->post_resp(RESP_ASR_FINAL, text);         /* 可靠: 逐条处理 */
    } else {
        self->post_stream(STREAM_ASR_PARTIAL, text);   /* 流式: 只留最新 */
    }
}

void UiBridge::drain_queues(void)
{
    /* ★ 两个消息体各约 0.5KB, 而本函数跑在 lvgl 任务(栈 6K, 且 lv_timer_handler
     *   内部还会嵌套调用)里 → 放 static 里, 不从本来就紧张的栈上再抠 1KB。
     *   本函数只被 ui_timer_cb 调用(单线程上下文), 所以 static 是安全的。 */
    static stream_msg_t s;
    static resp_msg_t   r;

    /* ---- 流式通道: 覆盖队列, "非空"就等于"有变化", 拿到才刷 ---- */
    while (xQueueReceive(m_stream_q, &s, 0) == pdTRUE) {
        ESP_LOGI(TAG, "[流式] kind=%d text=\"%s\"", (int)s.kind, s.text);
        /* 阶段 3+ 在这里上屏, 例如:
         *   if (s.kind == STREAM_ASR_PARTIAL) lv_label_set_text(ui_metext, s.text);
         * ★ lv_label_set_text 内部不比较内容、总是 free+malloc+重绘,
         *   所以必须靠"队列非空才刷"来避免无谓开销 —— 不要自己加轮询。 */
    }

    /* ---- 控制通道: 可靠队列, 逐条处理 ---- */
    while (xQueueReceive(m_resp_q, &r, 0) == pdTRUE) {
        ESP_LOGI(TAG, "[结果] kind=%d t=%ums text=\"%s\"",
                 (int)r.kind, (unsigned)r.t_ms, r.text);
    }
}

/* lv_timer 回调。★ 本函数由 lv_timer_handler() 调用, 此时 LVGL 锁已被持有,
 * 所以只能调用**不带锁**的内部函数, 绝不能碰 lvgl_port_lock()。 */
static void ui_timer_cb(lv_timer_t *t)
{
    (void)t;
    UiBridge::get().drain_queues();
}

esp_err_t UiBridge::start_ui_timer(void)
{
    if (lv_timer_create(ui_timer_cb, UI_TIMER_PERIOD_MS, nullptr) == nullptr) {
        ESP_LOGE(TAG, "lv_timer_create 失败");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UI 消费 timer 已注册 (每 %dms 取一次队列, 本阶段只 log)",
             UI_TIMER_PERIOD_MS);
    return ESP_OK;
}
