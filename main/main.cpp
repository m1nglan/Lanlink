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
#include "drivers/llm.hpp"

static const char *TAG = "Main";

/* ================================================================
 * 总体架构(双核三任务):
 *   CPU0: ws_task      WiFi 连接 + WebSocket 长连接 + 发送 start/音频/end
 *                      + 定时 ping 保活 + LLM 转发阶段机(切 openclaw/等回复/切回)
 *   CPU1: button_task  按键检测(长按 0.5s 确认防误触) + 录音状态机
 *   CPU1: i2s_task     I2S 采集 PCM → 写 StreamBuffer
 *
 * 跨任务通信:
 *   - 音频: i2s_task → s_audio_buf(StreamBuffer) → ws_task
 *   - 状态: s_asr_state(volatile 状态机), 各任务读/写
 *   - LLM:  s_llm_pending / s_reply_received / s_llm_busy(volatile)
 * ================================================================ */

/* 按键引脚(按下为高,外部下拉) */
#define BTN_PIN GPIO_NUM_10

/* ============ 录音状态机(三态) ============
 * IDLE      空闲,可开始录音
 * RECORDING 录音中(按住期间一直采集发音频)
 * WAITING   已发 end,等待服务器返回 final(此期间不可再次录音)
 * 转换: IDLE --长按0.5s--> RECORDING --松开--> WAITING --收到final--> IDLE
 */
typedef enum {
    ASR_STATE_IDLE,
    ASR_STATE_RECORDING,
    ASR_STATE_WAITING,
} asr_state_t;

static volatile asr_state_t s_asr_state = ASR_STATE_IDLE;   /* 共享录音状态机 */
static StreamBufferHandle_t s_audio_buf;                     /* 音频流缓冲(i2s→ws) */

/* LLM 转发状态(跨任务,volatile):
 *   s_final_text      本轮语音识别出的 final 完整文本(待转发给 LLM)
 *   s_llm_pending     有文本待转发给 LLM(收到 final 时置位,ws_task 消费)
 *   s_reply_received  已收到 LLM 的完整回复(reply 消息置位)
 *   s_llm_busy        LLM 转发流程进行中(切服务/等回复),期间禁止开始录音 */
static char s_final_text[512] = {0};
static volatile bool s_llm_pending = false;
static volatile bool s_reply_received = false;
static volatile bool s_llm_busy = false;

/* 结果回调(ws 事件上下文,由 RtAsr 调用):
 *   收到语音识别 final → 录音状态机回 IDLE, 保存文本并置 LLM 转发标志 */
static void on_result(const char *text, bool is_final, void *ctx)
{
    (void)ctx;
    if (is_final) {
        s_asr_state = ASR_STATE_IDLE;   /* 识别结束,录音状态机回空闲 */
        strlcpy(s_final_text, text, sizeof(s_final_text));   /* 保存最终识别文本 */
        s_llm_pending = true;           /* 标记有待转发的 LLM 文本 */
        ESP_LOGI(TAG, "收到最终结果,准备转发给 LLM");
    }
}

/* LLM 回复回调(ws 事件上下文,由 Llm 调用):
 *   只置已回复标志(通知 ws_task 的 LLM 阶段机可以切回 text),
 *   回复内容由 Llm::handle_reply 统一打印,这里不重复输出 */
static void on_llm_reply(const char *text, void *ctx)
{
    (void)text;
    (void)ctx;
    s_reply_received = true;
}

/* 按住确认时长: 超过该时长才真正开始录音会话(防误触) */
#define BTN_HOLD_MS (500)

/* 按键任务(核1): Button 驱动 + 录音状态机
 * 作用: 检测按键, 驱动 s_asr_state 在 IDLE/RECORDING/WAITING 间切换 */
static void button_task(void *arg)
{
    (void)arg;
    Button btn(BTN_PIN);
    ESP_ERROR_CHECK(btn.init());

    while (1) {
        bool pressed = btn.is_pressed();   /* 阻塞消抖 ~50ms */

        /* 条件: 按下 + 空闲 + LLM 未忙 → 尝试开始录音 */
        if (pressed && s_asr_state == ASR_STATE_IDLE && !s_llm_busy) {
            /* 按住确认: 保持 0.5s 才真正开始,短按忽略(防误触) */
            uint32_t start_ms = (uint32_t)(esp_timer_get_time() / 1000);
            bool confirmed = false;
            while (btn.is_pressed()) {   /* 循环等待按键保持 */
                if ((uint32_t)(esp_timer_get_time() / 1000) - start_ms >= BTN_HOLD_MS) {
                    confirmed = true;    /* 按住超过 0.5s,确认开始 */
                    break;
                }
            }
            if (confirmed) {
                s_asr_state = ASR_STATE_RECORDING;
                ESP_LOGI(TAG, "开始录音");
            }
            /* 未确认(短按松开): 不进入 RECORDING,本次忽略 */
        } else if (!pressed && s_asr_state == ASR_STATE_RECORDING) {
            /* 松开按键: 录音结束 → WAITING, 等 ws_task 发 end 和服务器 final */
            s_asr_state = ASR_STATE_WAITING;
            ESP_LOGI(TAG, "\n停止录音,等待结果");
        }
    }
}

/* i2s 任务(核1): 读状态机,录音时采集写 Stream Buffer
 * 作用: 持续从 I2S 麦克风读 PCM 帧(40ms/帧), 仅在 RECORDING 时写入缓冲 */
static void i2s_task(void *arg)
{
    (void)arg;
    I2sMic mic;
    ESP_ERROR_CHECK(mic.init());
    ESP_ERROR_CHECK(mic.start());

    while (1) {
        static int16_t pcm[I2S_MIC_FRAME_BYTES / 2];  /* static,不在栈上(防栈溢出) */
        if (mic.read_frame(pcm, sizeof(pcm), 100) == ESP_OK) {
            if (s_asr_state == ASR_STATE_RECORDING) {
                /* 录音中: 把 PCM 帧放入缓冲,由 ws_task 取出发送 */
                xStreamBufferSend(s_audio_buf, pcm, sizeof(pcm), 0);
            }
            /* 非录音时丢弃,保持 DMA 缓冲不溢出 */
        }
    }
}

/* ws 任务(核0,独占): WiFi + 长连接 + 语音收发 + LLM 转发
 * 主循环每轮依次检查: 连接状态 → ping → 录音流程(start/音频/end) → LLM 转发 → 超时兜底
 * 全部非阻塞(每个步骤只做一点,靠循环推进),避免卡住其他逻辑 */
static void ws_task(void *arg)
{
    (void)arg;
    WS &ws = WS::get();                       /* 共享 WebSocket 长连接(单例) */
    RtAsr asr;                                /* 语音听写业务驱动 */
    asr.set_result_callback(on_result, NULL); /* 收到 final 时触发 on_result */
    asr.attach(ws);                           /* 绑定共享连接,注册 partial/final/revise handler */

    Llm llm;                                  /* LLM 对话业务驱动 */
    llm.set_reply_callback(on_llm_reply, NULL);/* 收到 reply 时触发 on_llm_reply */
    llm.attach(ws);                           /* 绑定共享连接,注册 partial/reply handler */

    bool started = false;   /* 本轮是否已发 start */
    bool end_sent = false;  /* 本轮是否已发 end */
    uint32_t last_ping_ms = 0;  /* 上次发应用层 ping 的时间 */
    uint32_t end_time_ms = 0;   /* 上次发 end 的时间(用于 WAITING 超时兜底) */
    const uint32_t WAIT_FINAL_TIMEOUT_MS = 10000;  /* 发 end 后最多等 10s 的语音 final */
    const uint32_t LLM_REPLY_TIMEOUT_MS = 120000;  /* 等 OpenClaw 回复最长 120s(明岚工具调用可能很慢) */

    /* LLM 转发阶段机(非阻塞,靠主循环逐步推进,避免阻塞 ws_task):
     *   LLM_IDLE      无待转发
     *   LLM_SWITCHING 收到 final,已切到 openclaw 服务
     *   LLM_CHATTING  已发 chat,等 reply(收到 reply 或超时则下一阶段)
     *   LLM_BACK      已收到 reply/超时,切回 text 服务 */
    enum {
        LLM_IDLE,
        LLM_SWITCHING,
        LLM_CHATTING,
        LLM_BACK,
    } llm_stage = LLM_IDLE;
    uint32_t llm_stage_ms = 0;   /* 进入当前阶段的时刻(用于各阶段计时) */

    while (1) {
        /* 步骤 0: 确保连接。未连接 或 is_stale(超过 WS_STALE_TIMEOUT_MS 无数据)
         * 则销毁并重建连接。OpenClaw 空窗期长,is_stale 阈值(130s)已调大避免误判。 */
        if (!ws.is_connected() || ws.is_stale()) {
            ws.deinit();
            started = false;
            end_sent = false;
            xStreamBufferReset(s_audio_buf);
            if (ws.is_stale()) {
                ESP_LOGW(TAG, "连接超时无数据,重连中...");
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (ws.init() == ESP_OK) {
                asr.attach(ws);   /* 连接重建后重新绑定 handler */
                ESP_LOGI(TAG, "网关连接成功(长连接)");
                /* 切到 text 服务(服务器要求连接后显式切换,否则不处理 start/音频) */
                asr.switch_service("text", WS_SEND_TIMEOUT_MS);
            }
            continue;   /* 本次循环到此,等下次再走业务 */
        }

        /* 步骤 0.5: 定时发应用层 ping 保活(服务器回 pong,刷新 is_stale 判断) */
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - last_ping_ms >= (uint32_t)(WS_PING_INTERVAL_SEC * 1000)) {
            last_ping_ms = now_ms;
            ws.send_ping(WS_SEND_TIMEOUT_MS);
        }

        /* 步骤 1: 录音开始(RECORDING)且未发 start → 发 start(成功才置位,失败重试) */
        if (s_asr_state == ASR_STATE_RECORDING && !started) {
            if (asr.start(WS_SEND_TIMEOUT_MS) == ESP_OK) {
                started = true;
                end_sent = false;
                ESP_LOGI(TAG, "已发送 start");
            } else {
                vTaskDelay(pdMS_TO_TICKS(50));   /* 发送失败,稍后重试 */
            }
        }

        /* 步骤 2: 录音中发音频(从缓冲取 PCM 帧发给服务器,20ms 超时让出 CPU) */
        if (started && s_asr_state == ASR_STATE_RECORDING) {
            uint8_t data[I2S_MIC_FRAME_BYTES];
            size_t n = xStreamBufferReceive(s_audio_buf, data, sizeof(data), pdMS_TO_TICKS(20));
            if (n > 0) {
                asr.send_audio(data, n, WS_SEND_TIMEOUT_MS);
            }
        }

        /* 步骤 3: 录音结束(WAITING)且未发 end → 排空缓冲后发 end */
        if (started && s_asr_state == ASR_STATE_WAITING && !end_sent) {
            if (xStreamBufferIsEmpty(s_audio_buf)) {
                asr.end(WS_SEND_TIMEOUT_MS);
                end_sent = true;
                end_time_ms = (uint32_t)(esp_timer_get_time() / 1000);  /* 记录用于超时兜底 */
                ESP_LOGI(TAG, "已发送 end");
            } else {
                /* 缓冲还有音频,先发完再 end(避免漏发最后几帧) */
                uint8_t data[I2S_MIC_FRAME_BYTES];
                size_t n = xStreamBufferReceive(s_audio_buf, data, sizeof(data), pdMS_TO_TICKS(20));
                if (n > 0) {
                    asr.send_audio(data, n, WS_SEND_TIMEOUT_MS);
                }
            }
        }

        /* 步骤 4: final 后状态机已回 IDLE(on_result 置位),重置本轮标志 */
        if (started && s_asr_state == ASR_STATE_IDLE) {
            started = false;
            end_sent = false;
        }

        /* 步骤 4.5: LLM 转发阶段机(语音识别出的文本自动发给 OpenClaw)。
         * 非阻塞实现: 每个阶段只在主循环的一轮里做一小步,靠阶段状态推进。 */
        if (llm_stage == LLM_IDLE && s_llm_pending && ws.is_connected()) {
            /* LLM_IDLE + 有待转发文本 → 进入切换服务阶段 */
            llm_stage = LLM_SWITCHING;
            llm_stage_ms = (uint32_t)(esp_timer_get_time() / 1000);
            s_reply_received = false;    /* 复位"已回复"标志 */
            s_llm_busy = true;           /* 标记 LLM 忙,禁止按键开始录音 */
            llm.reset_stream();          /* 清空流式累积 buffer */
            ws.set_service("openclaw");  /* 通知 WS: 当前在 openclaw 服务(partial 分流) */
            ESP_LOGI(TAG, "切换到 OpenClaw...");
            llm.switch_service("openclaw", WS_SEND_TIMEOUT_MS);
        }
        if (llm_stage == LLM_SWITCHING) {
            /* 等 300ms 让服务器完成 svc 切换,再发 chat */
            if ((uint32_t)(esp_timer_get_time() / 1000) - llm_stage_ms >= 300) {
                llm.chat(s_final_text, WS_SEND_TIMEOUT_MS);
                ESP_LOGI(TAG, "已发送给 OpenClaw: %s", s_final_text);
                s_llm_pending = false;   /* 文本已消费 */
                llm_stage = LLM_CHATTING;
                llm_stage_ms = (uint32_t)(esp_timer_get_time() / 1000);
            }
        }
        if (llm_stage == LLM_CHATTING) {
            /* 等 OpenClaw 回复: 收到 reply(s_reply_received) 或超时 → 进 BACK。
             * 期间 OpenClaw 的流式 partial 由 Llm::handle_partial 实时打印。 */
            if (s_reply_received) {
                llm_stage = LLM_BACK;
                llm_stage_ms = (uint32_t)(esp_timer_get_time() / 1000);
            } else if ((uint32_t)(esp_timer_get_time() / 1000) - llm_stage_ms >= LLM_REPLY_TIMEOUT_MS) {
                ESP_LOGW(TAG, "等待 OpenClaw 回复超时");
                llm_stage = LLM_BACK;
                llm_stage_ms = (uint32_t)(esp_timer_get_time() / 1000);
            }
        }
        if (llm_stage == LLM_BACK) {
            /* 本轮 LLM 结束: 切回 text 服务,清状态,允许下一轮录音 */
            llm.switch_service("text", WS_SEND_TIMEOUT_MS);
            ws.set_service("text");      /* 通知 WS 回到 text 服务 */
            ESP_LOGI(TAG, "已切回 text");
            llm_stage = LLM_IDLE;
            s_llm_busy = false;          /* LLM 结束,恢复允许录音 */
            /* 清录音残留标志,确保下一轮录音状态干净 */
            end_sent = false;
            end_time_ms = 0;
        }

        /* 步骤 4.6: WAITING 超时兜底。发 end 后 10s 没收到语音 final → 强制回 IDLE,
         * 避免状态机永久卡死在 WAITING 导致后续按键失效。
         * LLM 阶段机活动期间跳过(此时不是等语音 final,而是等 LLM 回复)。 */
        if (llm_stage == LLM_IDLE && s_asr_state == ASR_STATE_WAITING && end_sent && end_time_ms != 0) {
            if ((uint32_t)(esp_timer_get_time() / 1000) - end_time_ms > WAIT_FINAL_TIMEOUT_MS) {
                ESP_LOGW(TAG, "等待 final 超时,强制回空闲");
                s_asr_state = ASR_STATE_IDLE;
                started = false;
                end_sent = false;
                end_time_ms = 0;
            }
        }

        /* 步骤 5: 非录音状态让出 CPU(录音中由 xStreamBufferReceive 20ms 超时让出) */
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

    /* 音频流缓冲(约 6 帧,1280 字节/帧),i2s_task 写入,ws_task 读出 */
    s_audio_buf = xStreamBufferCreate(8192, 1);
    if (s_audio_buf == NULL) {
        ESP_LOGE(TAG, "Stream Buffer 创建失败");
        return;
    }

    /* 创建三个任务并固定核:
     *   ws_task     → 核0(网络收发)
     *   button_task → 核1(按键+状态机)
     *   i2s_task    → 核1(麦克风采集)
     * 优先级均 5,栈大小按需(ws 8192 含 JSON 解析,button/i2s 4096) */
    xTaskCreatePinnedToCore(ws_task, "ws", 8192, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(button_task, "button", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(i2s_task, "i2s", 4096, NULL, 5, NULL, 1);

    /* app_main 收尾: 任务创建后自身无用,删除释放栈 */
    vTaskDelete(NULL);
}
