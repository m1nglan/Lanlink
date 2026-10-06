#pragma once

#include "esp_err.h"

/* ================================================================
 * ws_keeper —— **WS 长连接的全部生命周期** (原 ws_task 的连接层部分)
 *
 * 只干三件事:
 *   ① 首次建连          ② 断线/死连接重连        ③ 应用层 ping 保活
 *
 * 不干的事 (都搬走了):
 *   ✗ 发 start / 音频 / end   → voice_task
 *   ✗ **切服务 (svc)**        → voice_task (会话开头切, 且等 svc_ok 确认)
 *      原因: 现在切服务和"发 start"在同一个任务里顺序执行, 且中间有 svc_ok 握手
 *            → "语音已经过去了但服务还没切" 这个场景不可能发生。
 *            (旧调度器是轮询式的, 才需要在重连时补一刀)
 *   ✗ LLM 转发阶段机          → 本阶段不做 (见 llm_chain.hpp)
 *   ✗ IO8 服务切换            → 按键只打日志 (main.cpp), 不投队列
 *
 * ⚠️ 组件配置里 disable_auto_reconnect / disable_pingpong_discon 都是 true,
 *    也就是说**重连和保活必须由本任务负责**, 别指望 esp_websocket_client。
 * ================================================================ */

/*! 创建 ws_keeper_task (CPU0, prio 5, 栈 8K)。
 *  必须与 websocket_task 同核 —— 它们共用 client->lock。 */
esp_err_t ws_keeper_start(void);
