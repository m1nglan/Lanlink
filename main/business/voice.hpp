#pragma once

#include "esp_err.h"

/* ================================================================
 * voice —— 命令驱动的语音会话任务 (取代旧 i2s_task + ws_task 的录音部分)
 * [链] 按键GPIO中断 → button_edge_isr → button_edge_timer_cb → on_button_edge
 *      → UiBridge::voice_start → voice_q →【voice_task】→ voice_session
 *        → I2sMic::read_frame / RtAsr::send_audio → WS → 网关
 * 常驻但空闲时阻塞在 voice_q 上 (portMAX_DELAY) → 零 CPU, 不是轮询。
 * 会话结构: 等连接 → 切 text + 等 svc_ok → asr.start → mic.start
 *           → 循环{读帧 → peek STOP/断线/超时 → 发包} → mic.stop + asr.end
 * ⚠️ I2S 通道由本任务启动时建一次, 每轮会话只 start/stop。
 * ⚠️ 全机唯一采音者就是本任务 —— i2s_mic 的读缓冲是 static, 只能有一个读者。
 * ================================================================ */

/*! 创建 voice_task (CPU0, prio 6, 栈 6K)。 */
esp_err_t voice_task_start(void);
