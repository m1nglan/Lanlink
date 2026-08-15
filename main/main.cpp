#include <stdio.h>
#include <string.h>

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

static const char *TAG = "Main";

/* 按键引脚(按下为高,外部下拉) */
#define BTN_PIN GPIO_NUM_10

/* ============ 录音状态机(三态) ============
 * IDLE      空闲,可开始录音
 * RECORDING 录音中
 * WAITING   已发 end,等待服务器返回 final(此期间不可再次录音)
 */
typedef enum {
    ASR_STATE_IDLE,
    ASR_STATE_RECORDING,
    ASR_STATE_WAITING,
} asr_state_t;

static volatile asr_state_t s_asr_state = ASR_STATE_IDLE;   /* 共享状态机 */
static StreamBufferHandle_t s_audio_buf;                     /* 音频流缓冲 */

/* 结果回调(ws 事件上下文): 收到 final 后回 IDLE,连接保持 */
static void on_result(const char *text, bool is_final, void *ctx)
{
    (void)text;
    (void)ctx;
    if (is_final) {
        s_asr_state = ASR_STATE_IDLE;
        ESP_LOGI(TAG, "收到最终结果,本轮结束");
    }
}

/* 按键任务(核1): Button 驱动 + 状态机 */
static void button_task(void *arg)
{
    (void)arg;
    Button btn(BTN_PIN);
    ESP_ERROR_CHECK(btn.init());

    while (1) {
        bool pressed = btn.is_pressed();   /* 阻塞消抖 ~50ms */

        if (pressed && s_asr_state == ASR_STATE_IDLE) {
            s_asr_state = ASR_STATE_RECORDING;
            ESP_LOGI(TAG, "开始录音");
        } else if (!pressed && s_asr_state == ASR_STATE_RECORDING) {
            s_asr_state = ASR_STATE_WAITING;
            ESP_LOGI(TAG, "\n停止录音,等待结果");
        }
    }
}

/* i2s 任务(核1): 读状态机,录音时采集写 Stream Buffer */
static void i2s_task(void *arg)
{
    (void)arg;
    I2sMic mic;
    ESP_ERROR_CHECK(mic.init());
    ESP_ERROR_CHECK(mic.start());

    while (1) {
        static int16_t pcm[I2S_MIC_FRAME_BYTES / 2];  /* static,不在栈上 */
        if (mic.read_frame(pcm, sizeof(pcm), 100) == ESP_OK) {
            if (s_asr_state == ASR_STATE_RECORDING) {
                xStreamBufferSend(s_audio_buf, pcm, sizeof(pcm), 0);
            }
            /* 非录音时丢弃,保持 DMA 缓冲不溢出 */
        }
    }
}

/* ws 任务(核0,独占): 长连接 + start/end 触发一轮识别 */
static void ws_task(void *arg)
{
    (void)arg;
    WS &ws = WS::get();
    RtAsr asr;
    asr.set_result_callback(on_result, NULL);
    asr.attach(ws);   /* 绑定共享连接 */

    bool started = false;   /* 本轮是否已发 start */
    bool end_sent = false;  /* 本轮是否已发 end */
    uint32_t last_ping_ms = 0;  /* 上次发应用层 ping 的时间 */
    uint32_t end_time_ms = 0;   /* 上次发 end 的时间(用于 WAITING 超时兜底) */
    const uint32_t WAIT_FINAL_TIMEOUT_MS = 10000;  /* 发 end 后最多等 10s 的 final */

    while (1) {
        /* 0. 确保连接: 未连接 或 已死(超时无数据)→ 重连 */
        if (!ws.is_connected() || ws.is_stale()) {
            ws.deinit();
            started = false;
            end_sent = false;
            xStreamBufferReset(s_audio_buf);
            /* 死连接等待,避免频繁重连 */
            if (ws.is_stale()) {
                ESP_LOGW(TAG, "连接超时无数据,重连中...");
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (ws.init() == ESP_OK) {
                asr.attach(ws);   /* 连接重建后重新绑定 */
                ESP_LOGI(TAG, "网关连接成功(长连接)");
                /* 切到 text 服务(服务器要求连接后显式切换,否则不处理 start/音频) */
                asr.switch_service("text", WS_SEND_TIMEOUT_MS);
            }
            continue;
        }

        /* 0.5 定时发应用层 ping 保活(服务器回 pong,刷新 is_stale 判断) */
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - last_ping_ms >= (uint32_t)(WS_PING_INTERVAL_SEC * 1000)) {
            last_ping_ms = now_ms;
            ws.send_ping(WS_SEND_TIMEOUT_MS);
        }

        /* 1. 录音开始(RECORDING)且未发 start → 发 start(成功才置位,失败重试) */
        if (s_asr_state == ASR_STATE_RECORDING && !started) {
            if (asr.start(WS_SEND_TIMEOUT_MS) == ESP_OK) {
                started = true;
                end_sent = false;
                ESP_LOGI(TAG, "已发送 start");
            } else {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }

        /* 2. 录音中发音频 */
        if (started && s_asr_state == ASR_STATE_RECORDING) {
            uint8_t data[I2S_MIC_FRAME_BYTES];
            size_t n = xStreamBufferReceive(s_audio_buf, data, sizeof(data), pdMS_TO_TICKS(20));
            if (n > 0) {
                asr.send_audio(data, n, WS_SEND_TIMEOUT_MS);
            }
        }

        /* 3. 录音结束(WAITING)且未发 end → 排空 buffer 后发 end */
        if (started && s_asr_state == ASR_STATE_WAITING && !end_sent) {
            if (xStreamBufferIsEmpty(s_audio_buf)) {
                asr.end(WS_SEND_TIMEOUT_MS);
                end_sent = true;
                end_time_ms = (uint32_t)(esp_timer_get_time() / 1000);
                ESP_LOGI(TAG, "已发送 end");
            } else {
                uint8_t data[I2S_MIC_FRAME_BYTES];
                size_t n = xStreamBufferReceive(s_audio_buf, data, sizeof(data), pdMS_TO_TICKS(20));
                if (n > 0) {
                    asr.send_audio(data, n, WS_SEND_TIMEOUT_MS);
                }
            }
        }

        /* 4. final 后回 IDLE,重置本轮标志 */
        if (started && s_asr_state == ASR_STATE_IDLE) {
            started = false;
            end_sent = false;
        }

        /* 4.5 WAITING 超时兜底:发 end 后 WAIT_FINAL_TIMEOUT_MS 没收到 final → 强制回 IDLE,
         * 避免状态机永久卡死在 WAITING 导致后续按键失效 */
        if (s_asr_state == ASR_STATE_WAITING && end_sent && end_time_ms != 0) {
            if ((uint32_t)(esp_timer_get_time() / 1000) - end_time_ms > WAIT_FINAL_TIMEOUT_MS) {
                ESP_LOGW(TAG, "等待 final 超时,强制回空闲");
                s_asr_state = ASR_STATE_IDLE;
                started = false;
                end_sent = false;
                end_time_ms = 0;
            }
        }

        /* 5. 非录音状态让出 CPU(录音中由 xStreamBufferReceive 20ms 超时让出) */
        if (s_asr_state != ASR_STATE_RECORDING) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
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

    /* 音频流缓冲(约 6 帧,1280 字节/帧) */
    s_audio_buf = xStreamBufferCreate(8192, 1);
    if (s_audio_buf == NULL) {
        ESP_LOGE(TAG, "Stream Buffer 创建失败");
        return;
    }

    /* ws 任务 → 核0 */
    xTaskCreatePinnedToCore(ws_task, "ws", 8192, NULL, 5, NULL, 0);
    /* 按键任务 → 核1 */
    xTaskCreatePinnedToCore(button_task, "button", 4096, NULL, 5, NULL, 1);
    /* i2s 任务 → 核1 */
    xTaskCreatePinnedToCore(i2s_task, "i2s", 4096, NULL, 5, NULL, 1);

    /* app_main 收尾,删除自身 */
    vTaskDelete(NULL);
}
