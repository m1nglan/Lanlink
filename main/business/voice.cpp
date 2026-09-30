#include "business/voice.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "business/ui_bridge.hpp"
#include "drivers/ws.hpp"

static const char *TAG = "voice";

/* ---------------- 任务参数 ----------------
 * ★ 核选择: 必须与 websocket_task 同核(CPU0) —— 发音频要抢 client->lock。 */
#define VOICE_TASK_STACK     (6 * 1024)
#define VOICE_TASK_PRIO      (6)        /*!< 高于 ws_keeper(5): 发音频不能被保活拖住 */
#define VOICE_TASK_CORE      (0)

#define VOICE_WAIT_CONN_MS   (15000)    /*!< 会话开始前等 WS 建连的超时 */
#define VOICE_SVC_WAIT_MS    (300)      /*!< 切 text 后等服务器回 svc_ok */
#define VOICE_FAKE_HOLD_MS   (1000)     /*!< 阶段2 假会话的"录音"时长 */

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ================================================================
 * 一轮语音会话
 *
 * 阶段 2 版: **假会话**, 不碰 I2S。
 * 阶段 3 版: 把下面第 ⑤ 步换成真正的采音循环, 骨架见注释。
 * ================================================================ */
static void voice_session(UiBridge &ub)
{
    WS             &ws      = WS::get();
    RtAsr          &asr     = ub.asr();
    QueueHandle_t   voice_q = ub.voice_queue();
    voice_cmd_msg_t m;

    ESP_LOGI(TAG, "=== 会话开始 (阶段2 假会话: 不采音) ===");
    ub.post_resp_status(RESP_ASR_STATUS, STARTED);

    /* ① 等连接就绪 (连接生命周期归 ws_keeper, 这里只等) */
    uint32_t t0 = now_ms();
    while (!ws.is_connected()) {
        if (now_ms() - t0 > VOICE_WAIT_CONN_MS) {
            ESP_LOGW(TAG, "等连接超时, 会话中止");
            ub.post_resp_status(RESP_ASR_STATUS, ABORTED);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* ② 切到 text 服务 —— ★ 两句必须成对, 见 ws_keeper.cpp 的说明
     *
     * 注: 这里**故意不清空队列**。残留的 STOP 不需要在这里清 ——
     *     voice_task 外层循环遇到空闲态的 STOP 会"忽略"掉,
     *     若为"快速点按(按下即松开)"而来, 则正好让本轮会话立刻结束, 行为正确。
     *     在会话开头盲目清队列反而会把用户真实的松开信号吃掉。 */
    ws.set_service("text");
    asr.switch_service("text", WS_SEND_TIMEOUT_MS);
    vTaskDelay(pdMS_TO_TICKS(VOICE_SVC_WAIT_MS));   /* 等服务器回 svc_ok */

    /* ③ 开始一轮识别 */
    if (asr.start(WS_SEND_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGE(TAG, "start 发送失败, 会话中止");
        ub.post_resp_status(RESP_ASR_STATUS, ABORTED);

        return;
    }
    ESP_LOGI(TAG, "已发送 start");

    /* ④ 阶段2: 假"采音" —— 空等 VOICE_FAKE_HOLD_MS, 或被 STOP 提前打断。
     *    阶段3 替换为:
     *      mic.init() 已在 voice_task 启动时完成; 这里 mic.start() + asr.start()
     *      static int16_t pcm[I2S_MIC_FRAME_BYTES / 2];
     *      while (1) {
     *          esp_err_t r = mic.read_frame(pcm, sizeof(pcm), 40);   // 阻塞等一帧
     *          // ★ 不管读没读到都先看 STOP/断线, 否则麦克风故障时会永远结束不了
     *          if (xQueueReceive(voice_q, &m, 0) == pdTRUE && m.cmd == CMD_VOICE_STOP) break;
     *          if (!ws.is_connected()) { abort = true; break; }
     *          if (r != ESP_OK) continue;   // ← 读失败时 pcm 还是上一帧旧数据, 绝不能发
     *          if (asr.send_audio((uint8_t *)pcm, sizeof(pcm), WS_SEND_TIMEOUT_MS) != ESP_OK) {
     *              abort = true; break;
     *          }
     *      }
     */
    bool got_stop = false;
    uint32_t t1 = now_ms();
    while (now_ms() - t1 < VOICE_FAKE_HOLD_MS) {
        if (xQueueReceive(voice_q, &m, 0) == pdTRUE && m.cmd == CMD_VOICE_STOP) {
            got_stop = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "假会话结束 (%s)", got_stop ? "收到 STOP" : "到时");

    /* ⑤ 结束本轮: 发 end; 阶段3 还要 mic.stop() */
    asr.end(WS_SEND_TIMEOUT_MS);
    ESP_LOGI(TAG, "已发送 end");

    ub.post_resp_status(RESP_ASR_STATUS, ENDED);
    /* 注意: 这里**不等 final** —— final 由 websocket_task 收到后经 asr 回调
     *       投进 resp_q, 由 UI 侧消费。本任务直接回队列睡觉。 */
}

/* ================================================================
 * 常驻任务: 阻塞在命令队列上
 * ================================================================ */
static void voice_task(void *arg)
{
    (void)arg;
    UiBridge        &ub      = UiBridge::get();
    QueueHandle_t    voice_q = ub.voice_queue();
    voice_cmd_msg_t  m;

    while (1) {
        /* ★ 空闲时睡在这里, 零 CPU 占用。由 xQueueSend 唤醒, 不是轮询。 */
        if (xQueueReceive(voice_q, &m, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (m.cmd) {  //< voice_q队列
        case CMD_VOICE_START:
            voice_session(ub);
            break;

        case CMD_VOICE_STOP:
            /* 会话外收到 STOP: 说明是快速点按后残留的, 忽略即可 */
            ESP_LOGI(TAG, "空闲状态收到 CMD_VOICE_STOP, 忽略");
            break;


        default:
            ESP_LOGW(TAG, "未知命令 cmd=%d", (int)m.cmd);
            break;
        }
    }
}

esp_err_t voice_task_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(voice_task, "voice",
                                            VOICE_TASK_STACK, nullptr,
                                            VOICE_TASK_PRIO, nullptr,
                                            VOICE_TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "voice_task 创建失败");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "voice_task 已启动 (CPU%d prio%d 栈%d)",
             VOICE_TASK_CORE, VOICE_TASK_PRIO, VOICE_TASK_STACK);
    return ESP_OK;
}
