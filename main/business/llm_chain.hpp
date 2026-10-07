#pragma once

#include <stdint.h>

/* ================================================================
 * LLM 转发链 —— **声明式预留, 本阶段不编译任何逻辑** (只有类型 + 常量 + 接线说明)
 *
 * [链] STREAM_ASR(is_final) →【本阶段机】→ switch_service + chat
 *      → Llm::handle_partial/reply → STREAM_LLM → 气泡
 *
 * 旧实现(AppFsm)的阶段流转, 供理解历史:
 *   LLM_IDLE →(m_llm_pending)→ LLM_SWITCHING →(等 300ms)→ LLM_CHATTING
 *            →(reply / 超时)→ LLM_BACK →(切回 text)→ LLM_IDLE
 * ================================================================ */

/* ---------- LLM 服务名 ----------
 * 服务器靠 {"type":"svc","service":"..."} 在三个服务间切换 */
#define LLM_SVC_OPENCLAW   "openclaw"    /*!< 默认(明岚) */
#define LLM_SVC_LLM        "llm"         /*!< DeepSeek */
#define LLM_SVC_TEXT       "text"        /*!< 语音听写(非 LLM) */

/* ---------- 转发阶段机 (预留) ---------- */
typedef enum {
    LLM_IDLE = 0,        /*!< 无待转发 */
    LLM_SWITCHING,       /*!< 已切到 LLM 服务, 等服务器就绪 */
    LLM_CHATTING,        /*!< 已发 chat, 等 reply */
    LLM_BACK,            /*!< 已收到 reply/超时, 切回 text */
} llm_stage_t;

/* ---------- 超时常量 (取自旧 AppFsm) ---------- */
#define LLM_SWITCH_WAIT_MS   (300)      /*!< 切服务后等服务器回 svc_ok */
#define LLM_REPLY_TIMEOUT_MS (120000)   /*!< 等 agent 回复超时(工具调用空窗可达几十秒) */

/* ================================================================
 * 将来接线的位置 (★ 别现在写) —— 已按 V2 协议更新
 *
 * ① STREAM_ASR 且 is_final=1 且阶段机 IDLE → 进 LLM_SWITCHING
 * ② 等 LLM_SWITCH_WAIT_MS → set_service(LLM_SVC_LLM) + switch_service(等 svc_ok) + chat()
 * ③ partial/reply → Llm::handle_partial / handle_reply (驱动侧已累积) → STREAM_LLM
 * ④ 结束/超时 → 切回 LLM_SVC_TEXT
 * ★ V2: partial 现在只属于 llm/openclaw (text 改用 "asr") → WS 侧不再需要"按 m_service
 *   分流"那套特判。set_service/switch_service 仍要成对, 理由变成"保持本地视图一致"。
 * ================================================================ */
