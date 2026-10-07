#pragma once

#include "esp_err.h"

/* ================================================================
 * ws_keeper —— **WS 长连接的全部生命周期** (原 ws_task 的连接层部分)
 * [链] 【ws_keeper_task】→ WS::is_connected / is_stale / reconnect / send_ping
 *      → post_resp_status(RESP_WS_STATUS) → resp_q → drain_queues
 * 只干三件事: ① 首次建连  ② 断线/死连接重连  ③ 应用层 ping 保活
 * 不干的: 发 start/音频/end → voice_task; LLM 阶段机 → llm_chain.hpp; 服务键 → main.cpp。
 * ★★ **重连后故意不切服务**: 切服务和发 start 现同在 voice_task 顺序执行、中间隔
 *    svc_ok 握手 → "语音过去了服务还没切"不可能发生。顺带: VOICE_SVC_ACKED 的唯一
 *    生产者变成 voice_session 自己, 不会被重连的确认误唤醒。
 * ⚠️ 组件配置 disable_auto_reconnect / disable_pingpong_discon 都是 true
 *    → **重连和保活必须由本任务负责**, 别指望 esp_websocket_client。
 * ================================================================ */

/*! 创建 ws_keeper_task (CPU0, prio 5, 栈 8K)。
 *  必须与 websocket_task 同核 —— 它们共用 client->lock。 */
esp_err_t ws_keeper_start(void);
