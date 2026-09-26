#include "business/ws_keeper.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "business/ui_bridge.hpp"
#include "drivers/ws.hpp"

static const char *TAG = "ws_keeper";

/* ---------------- 任务参数 ----------------
 * ★ 核选择: 必须与 websocket_task 同核(CPU0)。
 *   组件 TX/RX 共用一把 client->lock (CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK 未开),
 *   同核能把抢锁退化成核内短临界区, 跨核则会带上缓存同步开销。
 *   核号在 ws.cpp::init() 里通过 task_core_id_set/task_core_id 设定。 */
#define WS_KEEPER_TASK_STACK   (8 * 1024)
#define WS_KEEPER_TASK_PRIO    (5)
#define WS_KEEPER_TASK_CORE    (0)

#define WS_KEEPER_POLL_MS      (200)    /*!< 循环节拍: 保活检查周期 */
#define WS_KEEPER_RETRY_MS     (1000)   /*!< 重连前等待 */

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void ws_keeper_task(void *arg)
{
    (void)arg;
    WS    &ws  = WS::get();
    RtAsr &asr = UiBridge::get().asr();

    uint32_t last_ping_ms   = 0;
    bool     first_attempt  = true;

    while (1) {
        /* ============ 断线 / 死连接 → 重建 ============ */
        if (!ws.is_connected() || ws.is_stale()) {
            ws.deinit();                 /* m_ws==NULL 时是安全的空操作 */
            last_ping_ms = 0;

            if (!first_attempt) {
                /* 首轮不打扰: 开机本来就没连上, 不算"重连" */
                UiBridge::get().post_resp(RESP_WS_STATUS, "重连中...");
                ESP_LOGW(TAG, "连接已失效, %.1fs 后重连", WS_KEEPER_RETRY_MS / 1000.0f);
                vTaskDelay(pdMS_TO_TICKS(WS_KEEPER_RETRY_MS));
            }
            first_attempt = false;

            if (ws.init() != ESP_OK) {
                ESP_LOGW(TAG, "建连失败, %.1fs 后重试", WS_KEEPER_RETRY_MS / 1000.0f);
                vTaskDelay(pdMS_TO_TICKS(WS_KEEPER_RETRY_MS));
                continue;
            }

            /* ★★ 这两句必须**成对**, 缺一不可 ——
             *   ① set_service 决定 partial 消息往哪个槽位路由 (语音 vs LLM)
             *   ② switch_service 才是"通知服务器切服务"
             *   漏掉 ① 的后果是**静默的**: 语音 partial 会被送去 chat 槽位,
             *   而那个槽位本阶段是 NULL, ws.cpp 里 `if (h != NULL)` 不报错 →
             *   识别文字凭空消失, 串口一条日志都没有。
             *   (旧代码在 app_fsm.cpp:125-126 就是成对写的) */
            ws.set_service("text");
            asr.switch_service("text", WS_SEND_TIMEOUT_MS);

            last_ping_ms = now_ms();
            UiBridge::get().post_resp(RESP_WS_STATUS, "已连接 (服务=text)");
            ESP_LOGI(TAG, "网关已连接, 已切到 text 服务");
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
