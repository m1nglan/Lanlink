#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

#include "drivers/ws.hpp"
#include "drivers/rtasr.hpp"
#include "drivers/llm.hpp"

/* ================================================================
 * AppFsm: 应用状态机模块
 *
 * 管理三个协作状态机, 把 ws_task 的业务逻辑从 main.cpp 抽离:
 *   1. 录音状态机   (IDLE/RECORDING/WAITING)   —— 按键驱动
 *   2. LLM 转发阶段机 (IDLE/SWITCHING/CHATTING/BACK) —— 语音识别后自动转发
 *   3. 服务选择     (openclaw/llm)             —— IO8 按键切换
 *
 * main.cpp 只负责:
 *   - 创建任务
 *   - 回调接线(把 AppFsm 的共享状态暴露给 button/i2s 任务)
 * ================================================================ */

/* ---------- 录音状态机 ---------- */
typedef enum {
    ASR_STATE_IDLE,       /* 空闲,可开始录音 */
    ASR_STATE_RECORDING,  /* 录音中(按住期间一直采集发音频) */
    ASR_STATE_WAITING,    /* 已发 end,等待服务器返回 final(此期间不可再次录音) */
} asr_state_t;

/* ---------- LLM 转发阶段机 ---------- */
typedef enum {
    LLM_IDLE,       /* 无待转发 */
    LLM_SWITCHING,  /* 收到 final,已切到 s_llm_service 服务 */
    LLM_CHATTING,   /* 已发 chat,等 reply(收到 reply 或超时则下一阶段) */
    LLM_BACK,       /* 已收到 reply/超时,切回 text 服务 */
} llm_stage_t;

class AppFsm {
public:
    /*! 绑定共享连接 + 注册回调(在 ws.init 成功后调用) */
    void attach(WS &ws, RtAsr &asr, Llm &llm);

    /*! 绑定音频流缓冲(由外部创建,传指针给 i2s_task/ws_task 用) */
    void set_audio_buf(StreamBufferHandle_t buf) { m_audio_buf = buf; }

    /*! 主循环步进: 每轮执行连接管理/ping/录音收发/LLM 转发/超时兜底。
     * 非阻塞, 应在 ws_task 的 while(1) 里每轮调用。 */
    void tick(void);

    /* ---------- 供按键任务访问的共享状态 ---------- */
    asr_state_t get_state(void) const { return m_asr_state; }
    bool is_llm_busy(void) const { return m_llm_busy; }
    void set_svc_switch_pending(void) { m_svc_switch_pending = true; }
    bool is_recording(void) const { return m_asr_state == ASR_STATE_RECORDING; }

    /*! 按键任务调用: 检测按键,驱动录音状态机(IO10 长按/松开) */
    void handle_button(bool press_hold);   /* press_hold=true 表示 IO10 处于按下 */

    /*! 录音状态机: 供按键任务确认长按后调用开始录音 */
    void begin_recording(void) { m_asr_state = ASR_STATE_RECORDING; }
    void end_recording(void) { m_asr_state = ASR_STATE_WAITING; }

    /*! 当前 LLM 转发服务("openclaw"/"llm") */
    const char *llm_service(void) const { return m_llm_service; }

private:
    /* 回调(ws 事件上下文) */
    static void on_result(const char *text, bool is_final, void *ctx);
    static void on_llm_reply(const char *text, void *ctx);

    /* 录音状态机 */
    volatile asr_state_t m_asr_state = ASR_STATE_IDLE;
    StreamBufferHandle_t m_audio_buf = NULL;

    /* LLM 转发状态 */
    char m_final_text[512] = {0};
    volatile bool m_llm_pending = false;
    volatile bool m_reply_received = false;
    volatile bool m_llm_busy = false;
    const char *m_llm_service = "openclaw";   /* 默认 OpenClaw(明岚) */
    volatile bool m_svc_switch_pending = false;

    /* ws_task 局部状态 */
    WS *m_ws = nullptr;
    RtAsr *m_asr = nullptr;
    Llm *m_llm = nullptr;
    bool m_started = false;    /* 本轮是否已发 start */
    bool m_end_sent = false;   /* 本轮是否已发 end */
    uint32_t m_last_ping_ms = 0;   /* 上次发 ping 时间 */
    uint32_t m_end_time_ms = 0;    /* 上次发 end 时间(WAITING 超时兜底) */
    llm_stage_t m_llm_stage = LLM_IDLE;
    uint32_t m_llm_stage_ms = 0;   /* 进入当前 LLM 阶段的时刻 */

    /* 常量 */
    static const uint32_t WAIT_FINAL_TIMEOUT_MS = 10000;
    static const uint32_t LLM_REPLY_TIMEOUT_MS = 120000;
};
