#pragma once

#include "esp_err.h"

/* ================================================================
 * ask_router —— "什么时候把哪段识别文本发给哪个服务"这条策略。
 *
 * ★ 本模块**不认识 LVGL、不认识队列**: 三个动作全部靠 init() 注入的回调执行。
 * ★ 触发优先级 (三条路共用 s_asked 闸, 保证一轮只问一次):
 *     ① 收到 final        → 立刻问 (文本最准)
 *     ② ENDED 后 2.5 秒   → 用最后一条 asr 兜底问
 *     ③ 下一轮开始前      → 若上一轮还没问出去, 补问
 * ★ 切服务后**必须等 svc_ok** 再发 chat —— 服务不对时 chat 会被网关 echo
 *   静默吞掉 (实测过), 现象是"气泡建了却永远等不到回复", 极难排查。
 *
 * ★ 线程上下文:
 *     on_asr / handle_svc_ok  → 跑在 websocket_task (持 client->lock), 只做非阻塞动作
 *     remember_last_asr / tick / on_session_start / on_session_end → 跑在 LVGL 任务
 * ================================================================ */

namespace ask_router {

/*! 注入的三个动作 —— 让本模块不依赖 UiBridge / WS / 流式队列。
 *  ★ 用 C 风格函数指针 (不能是捕获 lambda) → 由 UiBridge 的 static 成员实现。 */
typedef struct {
    /*! 切服务: 发 {"type":"svc","service":...}。返回 ESP_OK = 已发出, 开始等 svc_ok。 */
    esp_err_t (*switch_service)(const char *service);
    /*! 真正把问题发出去 —— **只在收到 svc_ok 之后**才调用。 */
    esp_err_t (*send_chat)(const char *content);
    /*! 放占位灰泡 (走 stream_q, 不碰 LVGL)。文字由本模块给 (ASK_WAIT_TEXT),
     *  免得那个常量在两个文件里各写一份。 */
    void (*show_waiting)(const char *text);
} hooks_t;

/*! 注册回调。必须在任何其它接口之前调用一次。 */
void init(const hooks_t *hooks);

/*! 会话开始: 先把上一轮还没问出去的补发掉, 再重置状态。
 *  ⚠️ 必须在 chat_view::begin_asr() **之前**调用 (原实现就是这个顺序)。 */
void on_session_start(void);

/*! 会话结束 (ENDED / ABORTED): 起"等 final"的兜底表。
 *  ⚠️ 必须在 chat_view::end_asr() **之后**调用。 */
void on_session_end(void);

/*! 收到一条 ASR 文本。is_final=true 时立刻转发 (文本最准的触发点)。 */
void on_asr(const char *text, bool is_final);

/*! 记下最近一条 ASR 文本 —— 兜底 tick 用的就是它。
 *  ★ 由 drain_queues 在 LVGL 任务里调, 与 tick 同上下文, 无竞争。
 *    (不在 on_asr 里更新, 因为那个跑在 WS 回调, 会和 tick 抢这个缓冲) */
void remember_last_asr(const char *text);

/*! 每轮调一次 (drain_queues 里): 超时兜底检查。 */
void tick(void);

/*! 由 UiBridge::on_svc_ok 转发进来。
 *  @return true  = 这个 svc_ok 是本模块要的 (已处理, 调用者别再往下走);
 *          false = 不是本模块的, 调用者继续自己的逻辑 (目前是投 VOICE_SVC_ACKED)。 */
bool handle_svc_ok(const char *service);

}  // namespace ask_router
