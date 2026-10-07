#include "business/ws_keeper.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "business/ui_bridge.hpp"
#include "drivers/ws.hpp"

static const char *TAG = "ws_keeper";

/* ---------------- 任务参数 ----------------
 * ★ 核选择: 必须与 websocket_task 同核(CPU0) —— TX/RX 共用一把 client->lock
 *   (CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK 未开), 同核把抢锁退化成核内短临界区。 */
#define WS_KEEPER_TASK_STACK   (8 * 1024)
#define WS_KEEPER_TASK_PRIO    (5)
#define WS_KEEPER_TASK_CORE    (0)

#define WS_KEEPER_POLL_MS      (200)    /*!< 循环节拍: 保活检查周期 */
#define WS_KEEPER_RETRY_MS     (1000)   /*!< 重连前等待 */

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* 上报 WS 状态给 UI。
 * ★ 去抖: 只在状态【变化】时投一条 —— 否则重连循环里每轮都投,
 *   resp_q 深度只有 16, 一秒一条很快就会满并开始丢消息。 */
static void report_ws_status(status_kind_t sta)
{
    static bool          inited = false;
    static status_kind_t last   = CONNECTED;   /* 初值随便, 由 inited 兜住 */

    if (inited && sta == last) {
        return;
    }
    inited = true;
    last   = sta;
    UiBridge::get().post_resp_status(RESP_WS_STATUS, sta);
}

static void ws_keeper_task(void *arg)
{
    (void)arg;
    WS &ws = WS::get();

    uint32_t last_ping_ms  = 0;
    bool     was_connected = false;   /* 用于区分"断线"和"开机就没连上" */

    while (1) {
        /* ============ 断线 / 死连接 → 重建 ============ */
        if (!ws.is_connected() || ws.is_stale()) {
            /* 只有"连上过又断了"才算 DISCONNECTED;
             * 开机就没连上不报, 免得 UI 一上来就显示"断线" */
            if (was_connected) {
                report_ws_status(DISCONNECTED);
                was_connected = false;
                ESP_LOGW(TAG, "连接已失效, %.1fs 后重连", WS_KEEPER_RETRY_MS / 1000.0f);
                vTaskDelay(pdMS_TO_TICKS(WS_KEEPER_RETRY_MS));
            }

            ws.deinit();                 /* m_ws==NULL 时是安全的空操作 */
            last_ping_ms = 0;

            if (ws.init() != ESP_OK) {
                report_ws_status(RECONNECTING);   /* 去抖后只报一次 */
                ESP_LOGW(TAG, "建连失败, %.1fs 后重试", WS_KEEPER_RETRY_MS / 1000.0f);
                vTaskDelay(pdMS_TO_TICKS(WS_KEEPER_RETRY_MS));
                continue;
            }

            /* ★★ 这里**故意不切服务** (旧代码重连后会 set_service + switch_service)。
             *   那是轮询式 FSM 的补丁: 怕"语音已经过去了但服务还没切"。现在切服务和
             *   发 start 都在 voice_task 里顺序执行、中间隔 svc_ok 握手 → 不可能发生。
             *   顺带好处: VOICE_SVC_ACKED 的唯一生产者变成 voice_session 自己,
             *   不会被重连的确认误唤醒。路由用的 WS::m_service 默认就是 "text"。 */

            last_ping_ms  = now_ms();
            was_connected = true;
            report_ws_status(CONNECTED);
            ESP_LOGI(TAG, "网关已连接 (服务由 voice_task 在会话开头切换)");
            continue;   /* 立刻回去跑保活逻辑 */
        }

        /* ============ 已连接 → 定时应用层 ping 保活 ============
         * 组件的协议层 PONG 超时被 disable_pingpong_discon 关掉了,
         * 所以保活只能靠我们自己发 {"type":"ping"}。
         * 死连接判定靠 WS::is_stale(): 超过 WS_STALE_TIMEOUT_MS 没收到任何数据。 */
        uint32_t t = now_ms();
        if (t - last_ping_ms >= (uint32_t)(WS_PING_INTERVAL_SEC * 1000)) {
            last_ping_ms = t;
            if (ws.send_ping(WS_SEND_TIMEOUT_MS) != ESP_OK) {
                /* 不立即判死: 交给下轮的 is_stale() 判定(避免一次发送失败就重连) */
                ESP_LOGW(TAG, "ping 发送失败 (下轮检查连接状态)");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(WS_KEEPER_POLL_MS));
    }
}

esp_err_t ws_keeper_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(ws_keeper_task, "ws_keeper",
                                           WS_KEEPER_TASK_STACK, nullptr,
                                           WS_KEEPER_TASK_PRIO, nullptr,
                                           WS_KEEPER_TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "ws_keeper_task 创建失败");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ws_keeper_task 已启动 (CPU%d prio%d 栈%d)",
             WS_KEEPER_TASK_CORE, WS_KEEPER_TASK_PRIO, WS_KEEPER_TASK_STACK);
    return ESP_OK;
}
