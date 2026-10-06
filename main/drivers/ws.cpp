#include "drivers/ws.hpp"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"   /* pdMS_TO_TICKS / TickType_t */

#include "apikey.h"

static const char *TAG = "ws";

/* 接收累积缓冲: 放 PSRAM(段属性加在定义上)。WS 是单例,静态成员全局仅一份。 */
EXT_RAM_BSS_ATTR char WS::m_rx_buf[WS::RX_BUF_SIZE];

WS& WS::get(void)
{
    static WS instance;
    return instance;
}

esp_err_t WS::init(void)
{
    if (m_ws != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 构建 ws:// URI(token 鉴权放 query,统一根路径) */
    char uri[160];
    snprintf(uri, sizeof(uri),
             "ws://%s:%d" WS_PATH "?token=%s",
             WS_HOST, WS_PORT, SERVER_TOKEN);

    esp_websocket_client_config_t cfg = {};
    cfg.uri = uri;
    cfg.buffer_size = 8192;                         /*!< 接收缓冲(partial 又小又密,调大防丢) */
    cfg.network_timeout_ms = WS_CONNECT_TIMEOUT_MS;
    cfg.disable_auto_reconnect = true;              /*!< 断线交给上层重建(ws_keeper) */
    cfg.disable_pingpong_discon = true;             /*!< 禁用协议层 PONG 超时 abort(保活交给上层) */

    /* ★ 把组件的收包任务钉到 CPU0, 与 voice_task / ws_keeper 同核。
     *   理由: 组件 TX/RX **共用一把 client->lock**
     *   (CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK 未开), 同核能把抢锁退化成
     *   核内短临界区, 避免跨核缓存同步开销。
     *
     *   ⚠️ 必须同时设 task_core_id_set! 组件实现是:
     *        if (config->task_core_id_set) cfg->task_core_id = config->task_core_id;
     *        else                          cfg->task_core_id = tskNO_AFFINITY;   ← 默认不绑核
     *      因为 0 本身是合法核号, 没法用 0 表示"没设置", 所以组件专门加了
     *      一个 bool 开关(esp_websocket_client.h:129)。
     *      只写 task_core_id = 0 是**无效的**, 会被当成未设置而走 tskNO_AFFINITY。
     *
     *   task_prio 留 0: 组件默认会给 WEBSOCKET_TASK_PRIORITY(=5)。
     *   task_stack 留 0: 组件默认会给 WEBSOCKET_TASK_STACK(=4K)。 */
    cfg.task_core_id_set = true;
    cfg.task_core_id     = 0;
    cfg.task_prio        = 5;                       /*!< 与默认值一致, 写出来只为明确 */

    m_ws = esp_websocket_client_init(&cfg);
    if (m_ws == NULL) {
        ESP_LOGE(TAG, "esp_websocket_client_init 失败");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_websocket_register_events(m_ws, WEBSOCKET_EVENT_ANY,
                                                  &WS::ws_event_handler, this);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册事件失败: %s", esp_err_to_name(ret));
        esp_websocket_client_destroy(m_ws);
        m_ws = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "连接 %s ...", uri);
    ret = esp_websocket_client_start(m_ws);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动失败: %s", esp_err_to_name(ret));
        esp_websocket_client_destroy(m_ws);
        m_ws = NULL;
        return ret;
    }

    /* 等待 WebSocket 握手完成 */
    uint32_t deadline = (uint32_t)(esp_timer_get_time() / 1000) + WS_CONNECT_TIMEOUT_MS;
    while (!esp_websocket_client_is_connected(m_ws)) {
        if ((uint32_t)(esp_timer_get_time() / 1000) > deadline) {
            ESP_LOGE(TAG, "连接超时");
            esp_websocket_client_destroy(m_ws);
            m_ws = NULL;
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    m_connected = true;
    m_last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000);
    ESP_LOGI(TAG, "WebSocket 握手成功");
    return ESP_OK;
}

void WS::deinit(void)
{
    m_connected = false;
    m_last_rx_ms = 0;
    if (m_ws != NULL) {
        esp_websocket_client_stop(m_ws);
        esp_websocket_client_destroy(m_ws);
        m_ws = NULL;
    }
}

bool WS::is_stale(void) const
{
    if (!m_connected) {
        return true;   /* 未连接=需要重连 */
    }
    if (m_last_rx_ms == 0) {
        return true;
    }
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    return (now_ms - m_last_rx_ms) > WS_STALE_TIMEOUT_MS;
}

void WS::set_handler(const char *type_key, ws_msg_handler_t cb, void *ctx)
{
    /* 去重:同 type 已注册则覆盖,避免 attach 重复注册导致表满 */
    for (int i = 0; i < m_handler_count; i++) {
        if (strcmp(type_key, m_type_keys[i]) == 0) {
            m_handlers[i] = cb;
            m_ctxs[i] = ctx;
            return;
        }
    }
    if (m_handler_count >= MAX_HANDLERS) {
        ESP_LOGE(TAG, "handler 表已满");
        return;
    }
    m_type_keys[m_handler_count] = type_key;
    m_handlers[m_handler_count] = cb;
    m_ctxs[m_handler_count] = ctx;
    m_handler_count++;
    ESP_LOGI(TAG, "注册 handler: type=%s", type_key);
}

void WS::set_svc_ok_callback(ws_svc_ok_cb_t cb, void *ctx)
{
    m_svc_ok_cb  = cb;
    m_svc_ok_ctx = ctx;
}

/* ★★ 单位换算: esp_websocket_client 的 timeout 参数是 **RTOS ticks, 不是毫秒**
 *   (头文件 esp_websocket_client.h:279 明写 "Write data timeout in RTOS ticks";
 *    组件内部 :742 才是 `timeout * portTICK_PERIOD_MS`, 即它自己按 ticks 用)。
 *
 *   ⚠️ 不换算的后果: CONFIG_FREERTOS_HZ=100 时, 传 1000 会被当成 1000 ticks
 *      = **10000 ms = 10 秒**, 而代码里那个常量叫 WS_SEND_TIMEOUT_MS(1 秒)。
 *      10 秒的代价很具体: 音频发一帧阻塞 10 秒 → 采音循环 10 秒没读 DMA,
 *      而 DMA 缓冲只有 128ms → 这一整段音频全丢。
 *
 *   ⚠️ 下限必须夹到 1 tick: pdMS_TO_TICKS(1..9) == 0 @HZ=100,
 *      而组件里 timeout=0 表示"不等待"(拿不到锁立即失败), 不是"很短"。 */
static TickType_t ws_timeout_ticks(uint32_t timeout_ms)
{
    TickType_t t = pdMS_TO_TICKS(timeout_ms);
    return (t < 1) ? 1 : t;
}

esp_err_t WS::send_text(const char *text, uint32_t timeout_ms)
{
    if (m_ws == NULL || !m_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    int sent = esp_websocket_client_send_text(m_ws, text, (int)strlen(text),
                                              ws_timeout_ticks(timeout_ms));   /* ★ ms→ticks */
    if (sent < 0) {
        ESP_LOGE(TAG, "发送文本失败 ret=%d", sent);
        /* 不置 m_connected=false: 连接死活由 is_stale()(超时无数据)判断,
         * 避免一次 send 失败就触发重连循环 */
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t WS::send_bin(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (m_ws == NULL || !m_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    int sent = esp_websocket_client_send_bin(m_ws, (const char *)data, (int)len,
                                             ws_timeout_ticks(timeout_ms));    /* ★ ms→ticks */
    if (sent < 0) {
        ESP_LOGE(TAG, "发送二进制失败 ret=%d", sent);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t WS::send_ping(uint32_t timeout_ms)
{
    return send_text("{\"type\":\"ping\"}", timeout_ms);
}

void WS::ws_event_handler(void *handler_args, esp_event_base_t base,
                          int32_t event_id, void *event_data)
{
    (void)base;
    WS *self = static_cast<WS *>(handler_args);
    if (self == nullptr) {
        return;
    }

    esp_websocket_event_data_t *data = static_cast<esp_websocket_event_data_t *>(event_data);
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
        self->m_connected = true;
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        self->m_connected = false;
        break;
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "WEBSOCKET_EVENT_CLOSED");
        self->m_connected = false;
        break;
    case WEBSOCKET_EVENT_FINISH:
        ESP_LOGW(TAG, "WEBSOCKET_EVENT_FINISH");
        self->m_connected = false;
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WEBSOCKET_EVENT_ERROR");
        self->m_connected = false;
        break;
    case WEBSOCKET_EVENT_DATA:
        /* 任何收到数据(含 PONG/心跳)都刷新活跃时间,用于死连接检测 */
        self->m_last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (data != nullptr && data->op_code == WS_TRANSPORT_OPCODES_TEXT) {
            self->handle_data(data->data_ptr, data->data_len);
        }
        /* PING/PONG 由 esp_websocket_client 内部自动处理,无需干预 */
        break;
    default:
        break;
    }
}

void WS::handle_data(const char *payload, int len)
{
    if (payload == NULL || len <= 0) {
        return;
    }

    /* 1. 追加到累积缓冲(处理粘包: 一次 DATA 可能带多条 JSON) */
    if (m_rx_len + len > RX_BUF_SIZE - 1) {
        ESP_LOGW(TAG, "接收缓冲溢出,丢弃累积数据");
        m_rx_len = 0;
    }
    memcpy(m_rx_buf + m_rx_len, payload, len);
    m_rx_len += len;
    m_rx_buf[m_rx_len] = '\0';

    /* 2. 循环切出完整 JSON 消息(花括号配对,处理分帧/粘包) */
    int start = 0;
    while (start < m_rx_len) {
        int brace = 0;
        bool in_string = false;
        int i = start;
        for (; i < m_rx_len; i++) {
            char c = m_rx_buf[i];
            if (in_string) {
                if (c == '\\') { i++; continue; }   /* 跳过转义 */
                if (c == '"') in_string = false;
                continue;
            }
            if (c == '"') { in_string = true; }
            else if (c == '{') { brace++; }
            else if (c == '}') { brace--; }

            if (brace == 0 && i > start) {
                /* 找到一条完整 JSON: [start, i]
                 *
                 * ★★ 就地分发, **不再拷到 1KB 的栈缓冲里**。原因两条:
                 *   ① 长度上限: V2 下 LLM 的 {"type":"reply"} 可以带 2KB 正文,
                 *      JSON 会超过 1KB 拷贝缓冲 → 一截断 cJSON 就解析失败
                 *      → 整条回复被静默丢掉。就地分发没有长度上限。
                 *   ② 栈压力: websocket_task 栈只有 4K, 而回调链里
                 *      post_stream 还要放 2KB 的 stream_msg_t —— 省掉这 1KB 很值。
                 *
                 * cJSON_Parse 内部会把字符串复制走, 所以"临时把下一个字节改成
                 * '\0' → 分发 → 改回来"是安全的。 */
                char saved = m_rx_buf[i + 1];   /* i+1 <= m_rx_len, 而 m_rx_buf[m_rx_len] 就是 '\0' */
                m_rx_buf[i + 1] = '\0';
                dispatch_msg(m_rx_buf + start, i - start + 1);
                m_rx_buf[i + 1] = saved;
                start = i + 1;
                break;
            }
        }
        if (i >= m_rx_len) {
            /* 没有完整消息,剩余部分保留在缓冲 */
            break;
        }
    }

    /* 3. 移动未消费的剩余数据到缓冲头 */
    if (start > 0 && start < m_rx_len) {
        int remain = m_rx_len - start;
        memmove(m_rx_buf, m_rx_buf + start, remain);
        m_rx_len = remain;
        m_rx_buf[m_rx_len] = '\0';
    } else if (start >= m_rx_len) {
        m_rx_len = 0;   /* 全部消费完 */
    }
}

/* 解析单条完整 JSON 消息并分发 */
void WS::dispatch_msg(const char *msg, int len)
{
    cJSON *root = cJSON_Parse(msg);
    if (root == NULL) {
        ESP_LOGW(TAG, "JSON 解析失败: %s", msg);
        return;
    }
    const cJSON *type_item = cJSON_GetObjectItem(root, "type");
    if (type_item == NULL || !cJSON_IsString(type_item)) {
        ESP_LOGW(TAG, "消息无 type 字段: %s", msg);
        cJSON_Delete(root);
        return;
    }
    /* 先复制 type 到本地,再删 root(避免悬空指针) */
    char type[32];
    strlcpy(type, type_item->valuestring, sizeof(type));
    cJSON_Delete(root);

    /* 通用帧由 WS 自己处理 */
    if (strcmp(type, "error") == 0) {
        /* ★ 这里【故意不 return】—— 继续往下走查表分发。
         *   原因: 只打日志的话, 上层(voice)不知道"服务器已经放弃这一轮了",
         *   会继续白发音频。UiBridge 注册了 "error" 处理器, 由它决定怎么收尾。
         *   (type 是 "error", 不会命中下面的 svc_ok/pong, 自然落到查表。) */
        ESP_LOGE(TAG, "网关错误: %s", msg);
    }
    if (strcmp(type, "svc_ok") == 0) {
        ESP_LOGI(TAG, "服务切换成功: %s", msg);
        /* ★ 这才是"服务器已切好"的**真信号** —— 通知在等它的人。
         *   (voice_session 原来只能盲等 300ms, 服务器慢一点就会
         *    "在旧服务上发 start", 而那种错是静默的) */
        if (m_svc_ok_cb != NULL) {
            m_svc_ok_cb(m_service, m_svc_ok_ctx);
        }
        return;
    }
    if (strcmp(type, "pong") == 0) {
        ESP_LOGD(TAG, "pong");
        return;
    }

    /* ★ V2(PROTOCOL.md): 所有业务下行统一走下面这张表 ——
     *   "asr" / "final"     → RtAsr 注册的处理器
     *   "partial" / "reply" → Llm 注册的处理器 (未接入时就是未注册 → 丢弃)
     *   V1 因为 partial 被 text 和 llm 共用, 这里有一整套"按 m_service 分流"的特判;
     *   V2 里 asr 只属于 text、partial 只属于 llm, 一维 type 就能区分 → 特判已删。 */

    /* 查表分发 */
    for (int i = 0; i < m_handler_count; i++) {
        if (strcmp(type, m_type_keys[i]) == 0 && m_handlers[i] != NULL) {
            m_handlers[i](msg, len, m_ctxs[i]);
            return;
        }
    }

    ESP_LOGD(TAG, "未注册的 type=%s, 丢弃", type);
}
