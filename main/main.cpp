#include <stdio.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

#include "drivers/wifi.hpp"
#include "drivers/button.hpp"
#include "drivers/i2s_mic.hpp"
#include "drivers/ws.hpp"
#include "drivers/rtasr.hpp"
#include "drivers/llm.hpp"
#include "drivers/app_fsm.hpp"

static const char *TAG = "Main";

/* ================================================================
 * 总体架构(双核三任务):
 *   CPU0: ws_task      WiFi + 长连接 + 语音收发 + LLM 转发(调用 AppFsm::tick)
 *   CPU1: button_task  按键检测(IO10 长按录音 / IO8 切换服务)
 *   CPU1: i2s_task     I2S 采集 PCM → 写 StreamBuffer
 *
 * 业务状态机(录音/LLM/服务选择)全部封装在 AppFsm 驱动里,
 * 本文件只负责: 任务创建 + 回调接线 + 音频缓冲。
 * ================================================================ */

/* 按键引脚 */
#define BTN_PIN     GPIO_NUM_10   /* IO10: 录音键(按下为低,内部上拉) */
#define BTN_SVC_PIN GPIO_NUM_8    /* IO8:  服务切换键(按下为低,内部上拉) */
#define BTN_HOLD_MS (500)         /* 录音键长按确认时长(防误触) */

/* 全局: AppFsm 单例 + 音频缓冲 */
static AppFsm s_fsm;
static StreamBufferHandle_t s_audio_buf;

/* 按键任务(核1): 检测按键, 驱动 AppFsm 录音状态机 + IO8 服务切换 */
static void button_task(void *arg)
{
    (void)arg;
    Button btn(BTN_PIN);
    Button btn_svc(BTN_SVC_PIN);
    ESP_ERROR_CHECK(btn.init());
    ESP_ERROR_CHECK(btn_svc.init());

    while (1) {
        /* IO8 服务切换: 按下沿触发一次(长按不重复) */
        if (btn_svc.is_pressed_edge()) {
            s_fsm.set_svc_switch_pending();
        }

        /* IO10 录音: 长按 0.5s 确认开始, 松开结束 */
        bool pressed = btn.is_pressed();
        if (pressed && s_fsm.get_state() == ASR_STATE_IDLE && !s_fsm.is_llm_busy()) {
            uint32_t start_ms = (uint32_t)(esp_timer_get_time() / 1000);
            bool confirmed = false;
            while (btn.is_pressed()) {
                if ((uint32_t)(esp_timer_get_time() / 1000) - start_ms >= BTN_HOLD_MS) {
                    confirmed = true;
                    break;
                }
            }
            if (confirmed) {
                s_fsm.begin_recording();
                ESP_LOGI(TAG, "开始录音");
            }
        } else if (!pressed && s_fsm.get_state() == ASR_STATE_RECORDING) {
            s_fsm.end_recording();
            ESP_LOGI(TAG, "\n停止录音,等待结果");
        }
    }
}

/* i2s 任务(核1): 读状态机, 录音时采集写 Stream Buffer */
static void i2s_task(void *arg)
{
    (void)arg;
    I2sMic mic;
    ESP_ERROR_CHECK(mic.init());
    ESP_ERROR_CHECK(mic.start());

    while (1) {
        static int16_t pcm[I2S_MIC_FRAME_BYTES / 2];  /* static,不在栈上(防栈溢出) */
        if (mic.read_frame(pcm, sizeof(pcm), 100) == ESP_OK) {
            if (s_fsm.is_recording()) {
                xStreamBufferSend(s_audio_buf, pcm, sizeof(pcm), 0);
            }
            /* 非录音时丢弃,保持 DMA 缓冲不溢出 */
        }
    }
}

/* ws 任务(核0): WiFi + 长连接 + 语音收发 + LLM 转发(状态机在 AppFsm 驱动) */
static void ws_task(void *arg)
{
    (void)arg;
    WS &ws = WS::get();
    RtAsr asr;
    Llm llm;

    /* 回调接线 + 绑定缓冲 */
    s_fsm.attach(ws, asr, llm);
    s_fsm.set_audio_buf(s_audio_buf);

    while (1) {
        s_fsm.tick();   /* 每轮推进 AppFsm 状态机 */
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=========Main=========");
    ESP_LOGI(TAG, "空闲堆: %u 字节", (unsigned)esp_get_free_heap_size());

    /* 开机连接 WiFi(阻塞),之后断联重连由 wifi 驱动事件回调处理 */
    WiFi wifi;
    ESP_ERROR_CHECK(wifi.init());
    ESP_LOGI(TAG, "WiFi 连接成功");

    /* 音频流缓冲(约 6 帧,1280 字节/帧),i2s_task 写入,ws_task 读出 */
    s_audio_buf = xStreamBufferCreate(8192, 1);
    if (s_audio_buf == NULL) {
        ESP_LOGE(TAG, "Stream Buffer 创建失败");
        return;
    }

    /* 创建三个任务并固定核:
     *   ws_task     → 核0(网络收发)
     *   button_task → 核1(按键+状态机)
     *   i2s_task    → 核1(麦克风采集) */
    xTaskCreatePinnedToCore(ws_task, "ws", 8192, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(button_task, "button", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(i2s_task, "i2s", 4096, NULL, 5, NULL, 1);

    /* app_main 收尾: 任务创建后自身无用,删除释放栈 */
    vTaskDelete(NULL);
}
