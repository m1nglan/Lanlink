#pragma once

#include "esp_err.h"

/* ================================================================
 * voice —— 命令驱动的语音会话任务 (取代旧 i2s_task + ws_task 里的录音部分)
 *
 * 常驻, 但**空闲时阻塞在 cmd_q 上**(portMAX_DELAY) → 零 CPU 占用。
 * 不是轮询: 由按键回调 xQueueSend 唤醒。
 *
 * 会话结构 (阶段 3 的目标形态):
 *   等连接 → 切 text + 等 svc_ok → mic.start + asr.start
 *     → 循环 { mic.read_frame(40ms) → 帧尾 peek STOP → asr.send_audio }
 *     → asr.end + mic.stop → 回队列睡
 *
 * ⚠️ 本阶段(阶段2)的 voice_session 是**假会话**: 不碰 I2S,
 *    只验证 "切服务 → start → … → end" 这条协议链。
 * ================================================================ */

/*! 创建 voice_task (CPU0, prio 6, 栈 6K)。 */
esp_err_t voice_task_start(void);
