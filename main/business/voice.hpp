#pragma once

#include "esp_err.h"

/* ================================================================
 * voice —— 命令驱动的语音会话任务 (取代旧 i2s_task + ws_task 里的录音部分)
 *
 * 常驻, 但**空闲时阻塞在 voice_q 上**(portMAX_DELAY) → 零 CPU 占用。
 * 不是轮询: 由按键回调 xQueueSend 唤醒。
 *
 * 会话结构 (真采音):
 *   等连接 → 切 text + 等 svc_ok → asr.start → mic.start
 *     → 循环 { mic.read_frame(40ms) → 帧头 peek STOP/断线 → asr.send_audio }
 *     → mic.stop + asr.end → 回队列睡
 *
 * ⚠️ I2S 通道由 voice_task **启动时建一次**(init, 较重), 每轮会话只 start/stop。
 * ⚠️ 全机唯一采音者就是本任务 —— i2s_mic 的读缓冲是 static, 只能有一个读者。
 * ================================================================ */

/*! 创建 voice_task (CPU0, prio 6, 栈 6K)。 */
esp_err_t voice_task_start(void);
