#include "drivers/app_fsm.hpp"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "app_fsm";

/* ======================= 回调(ws 事件上下文) ======================= */

/* 收到语音识别 final → 录音状态机回 IDLE, 保存文本并置 LLM 转发标志 */
void AppFsm::on_result(const char *text, bool is_final, void *ctx)
{
    AppFsm *self = static_cast<AppFsm *>(ctx);
    if (self == nullptr) {
        return;
    }
    if (is_final) {
        self->m_asr_state = ASR_STATE_IDLE;   /* 识别结束,录音状态机回空闲 */
        strlcpy(self->m_final_text, text, sizeof(self->m_final_text));  /* 保存最终识别文本 */
        self->m_llm_pending = true;           /* 标记有待转发的 LLM 文本 */
        ESP_LOGI(TAG, "收到最终结果,准备转发给 LLM");
    }
}

/* 收到 LLM 完整回复 → 置已回复标志(回复内容由 Llm::handle_reply 打印) */
void AppFsm::on_llm_reply(const char *text, void *ctx)
{
    (void)text;
    AppFsm *self = static_cast<AppFsm *>(ctx);
    if (self == nullptr) {
        return;
    }
    self->m_reply_received = true;
}

/* ======================= 初始化 ======================= */

void AppFsm::attach(WS &ws, RtAsr &asr, Llm &llm)
{
    m_ws = &ws;
    m_asr = &asr;
    m_llm = &llm;

    asr.set_result_callback(&AppFsm::on_result, this);
    asr.attach(ws);

    llm.set_reply_callback(&AppFsm::on_llm_reply, this);
    llm.attach(ws);

    ESP_LOGI(TAG, "AppFsm 已绑定 WS/Asr/Llm");
}

/* ======================= 按键处理 ======================= */

void AppFsm::handle_button(bool press_hold)
{
    /* IO10 录音: 按下 + 空闲 + LLM 未忙 → 开始录音(长按确认由调用方 button_task 处理) */
    if (press_hold && m_asr_state == ASR_STATE_IDLE && !m_llm_busy) {
        m_asr_state = ASR_STATE_RECORDING;
        ESP_LOGI(TAG, "开始录音");
    } else if (!press_hold && m_asr_state == ASR_STATE_RECORDING) {
        /* 松开 → WAITING, 等 ws_task 发 end 和服务器 final */
        m_asr_state = ASR_STATE_WAITING;
        ESP_LOGI(TAG, "\n停止录音,等待结果");
    }
}

/* ======================= 主循环步进 ======================= */

void AppFsm::tick(void)
{
    WS &ws = *m_ws;
    RtAsr &asr = *m_asr;
    Llm &llm = *m_llm;

    /* 步骤 0: 确保连接。未连接 或 is_stale(超过 WS_STALE_TIMEOUT_MS 无数据)则重建。 */
    if (!ws.is_connected() || ws.is_stale()) {
        ws.deinit();
        m_started = false;
        m_end_sent = false;
        if (m_audio_buf != NULL) {
            xStreamBufferReset(m_audio_buf);
        }
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
        return;   /* 本次循环到此,等下次再走业务 */
    }

    /* 步骤 0.5: 定时发应用层 ping 保活 */
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (now_ms - m_last_ping_ms >= (uint32_t)(WS_PING_INTERVAL_SEC * 1000)) {
        m_last_ping_ms = now_ms;
        ws.send_ping(WS_SEND_TIMEOUT_MS);
    }

    /* 步骤 0.6: IO8 服务切换。LLM 阶段机空闲时才能切(避免打断 LLM 会话) */
    if (m_svc_switch_pending && m_llm_stage == LLM_IDLE && ws.is_connected()) {
        m_llm_service = (strcmp(m_llm_service, "openclaw") == 0) ? "llm" : "openclaw";
        ws.set_service(m_llm_service);
        llm.switch_service(m_llm_service, WS_SEND_TIMEOUT_MS);
        ESP_LOGI(TAG, "切换到 %s", m_llm_service);
        m_svc_switch_pending = false;
    }

    /* 步骤 1: 录音开始(RECORDING)且未发 start → 先确保 text 服务,再发 start */
    if (m_asr_state == ASR_STATE_RECORDING && !m_started) {
        ws.set_service("text");
        asr.switch_service("text", WS_SEND_TIMEOUT_MS);
        vTaskDelay(pdMS_TO_TICKS(300));   /* 等服务器完成 svc 切换并回 svc_ok */
        if (asr.start(WS_SEND_TIMEOUT_MS) == ESP_OK) {
            m_started = true;
            m_end_sent = false;
            ESP_LOGI(TAG, "已发送 start");
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));   /* 发送失败,稍后重试 */
        }
    }

    /* 步骤 2: 录音中发音频 */
    if (m_started && m_asr_state == ASR_STATE_RECORDING) {
        uint8_t data[I2S_MIC_FRAME_BYTES];
        size_t n = xStreamBufferReceive(m_audio_buf, data, sizeof(data), pdMS_TO_TICKS(20));
        if (n > 0) {
            asr.send_audio(data, n, WS_SEND_TIMEOUT_MS);
        }
    }

    /* 步骤 3: 录音结束(WAITING)且未发 end → 排空缓冲后发 end */
    if (m_started && m_asr_state == ASR_STATE_WAITING && !m_end_sent) {
        if (xStreamBufferIsEmpty(m_audio_buf)) {
            asr.end(WS_SEND_TIMEOUT_MS);
            m_end_sent = true;
            m_end_time_ms = (uint32_t)(esp_timer_get_time() / 1000);
            ESP_LOGI(TAG, "已发送 end");
        } else {
            /* 缓冲还有音频,先发完再 end */
            uint8_t data[I2S_MIC_FRAME_BYTES];
            size_t n = xStreamBufferReceive(m_audio_buf, data, sizeof(data), pdMS_TO_TICKS(20));
            if (n > 0) {
                asr.send_audio(data, n, WS_SEND_TIMEOUT_MS);
            }
        }
    }

    /* 步骤 4: final 后状态机已回 IDLE(on_result 置位),重置本轮标志 */
    if (m_started && m_asr_state == ASR_STATE_IDLE) {
        m_started = false;
        m_end_sent = false;
    }

    /* 步骤 4.5: LLM 转发阶段机(语音识别出的文本自动发给当前选中的服务) */
    if (m_llm_stage == LLM_IDLE && m_llm_pending && ws.is_connected()) {
        m_llm_stage = LLM_SWITCHING;
        m_llm_stage_ms = (uint32_t)(esp_timer_get_time() / 1000);
        m_reply_received = false;
        m_llm_busy = true;
        llm.reset_stream();
        ws.set_service(m_llm_service);
        ESP_LOGI(TAG, "发送给 %s ...", m_llm_service);
        llm.switch_service(m_llm_service, WS_SEND_TIMEOUT_MS);
    }
    if (m_llm_stage == LLM_SWITCHING) {
        if ((uint32_t)(esp_timer_get_time() / 1000) - m_llm_stage_ms >= 300) {
            llm.chat(m_final_text, WS_SEND_TIMEOUT_MS);
            ESP_LOGI(TAG, "已发送给 %s: %s", m_llm_service, m_final_text);
            m_llm_pending = false;
            m_llm_stage = LLM_CHATTING;
            m_llm_stage_ms = (uint32_t)(esp_timer_get_time() / 1000);
        }
    }
    if (m_llm_stage == LLM_CHATTING) {
        if (m_reply_received) {
            m_llm_stage = LLM_BACK;
            m_llm_stage_ms = (uint32_t)(esp_timer_get_time() / 1000);
        } else if ((uint32_t)(esp_timer_get_time() / 1000) - m_llm_stage_ms >= LLM_REPLY_TIMEOUT_MS) {
            ESP_LOGW(TAG, "等待 %s 回复超时", m_llm_service);
            m_llm_stage = LLM_BACK;
            m_llm_stage_ms = (uint32_t)(esp_timer_get_time() / 1000);
        }
    }
    if (m_llm_stage == LLM_BACK) {
        llm.switch_service("text", WS_SEND_TIMEOUT_MS);
        ws.set_service("text");
        ESP_LOGI(TAG, "已切回 text");
        m_llm_stage = LLM_IDLE;
        m_llm_busy = false;
        /* 清录音残留标志,确保下一轮录音状态干净 */
        m_end_sent = false;
        m_end_time_ms = 0;
    }

    /* 步骤 4.6: WAITING 超时兜底(LLM 阶段机活动期间跳过) */
    if (m_llm_stage == LLM_IDLE && m_asr_state == ASR_STATE_WAITING &&
        m_end_sent && m_end_time_ms != 0) {
        if ((uint32_t)(esp_timer_get_time() / 1000) - m_end_time_ms > WAIT_FINAL_TIMEOUT_MS) {
            ESP_LOGW(TAG, "等待 final 超时,强制回空闲");
            m_asr_state = ASR_STATE_IDLE;
            m_started = false;
            m_end_sent = false;
            m_end_time_ms = 0;
        }
    }

    /* 步骤 5: 非录音状态让出 CPU(录音中由 xStreamBufferReceive 20ms 超时让出) */
    if (m_asr_state != ASR_STATE_RECORDING) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
