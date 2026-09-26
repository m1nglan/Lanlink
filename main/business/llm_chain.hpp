#pragma once

#include <stdint.h>

/* ================================================================
 * LLM 转发链 —— **声明式预留, 本阶段不编译任何逻辑**
 *
 * REBUILD 的范围边界: "LLM 链本次不做, 枚举/常量声明式预留"。
 * 这里只放类型和常量 + 接线注释, 等阶段 3 之后真要做时,
 * 直接按下面的注释接线即可 (旧实现见 git 历史里的 app_fsm.cpp 步骤 4.5)。
 *
 * 旧实现(AppFsm)的阶段流转:
 *   LLM_IDLE →(看到 m_llm_pending)→ LLM_SWITCHING →(等 300ms)→ LLM_CHATTING
 *            →(收到 reply / 超时)→ LLM_BACK →(切回 text)→ LLM_IDLE
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
 * 将来接线的位置 (别现在写):
 *
 * ① 收到 RESP_ASR_FINAL → 若当前服务是 LLM 且阶段机 IDLE, 则进入 LLM_SWITCHING
 * ② LLM_SWITCHING 等 LLM_SWITCH_WAIT_MS 后:
 *        WS::get().set_service(LLM_SVC_LLM);          // ★ 决定 partial 往哪路由
 *        llm.switch_service(LLM_SVC_LLM, ...);        // ★ 通知服务器
 *        llm.chat(text, ...);
 * ③ 收到 {"type":"reply"} → Llm::handle_reply → 投 RESP_LLM_FINAL
 *    (流式增量走 {"type":"partial"}, 由 WS::m_service 路由到 m_partial_chat_cb)
 * ④ 回复结束/超时 → 切回 text:
 *        WS::get().set_service(LLM_SVC_TEXT);
 *        llm.switch_service(LLM_SVC_TEXT, ...);
 *
 * ⚠️ 每一步的 set_service + switch_service 必须**成对**, 否则 partial 静默丢失。
 * ================================================================ */
