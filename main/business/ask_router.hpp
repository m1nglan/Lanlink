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

/*! 现在"忙不忙" —— 给天气轮询判空闲用（忙就跳过这一轮，别抢 svc 状态）。
 *
 *  三个判据, 任一成立就算忙:
 *    ① 有提问在等 svc_ok     (s_pending)
 *    ② 在等 final 的宽限期里  (s_ask_armed && 还没问到)
 *    ③ 刚发完 chat 的冷却期内 (A'' 方案: 发出去后 30 秒内一律算忙)
 *
 *  ★★ 为什么 ③ 用【固定 30 秒】而不是"等 reply 到了再解锁":
 *    openclaw(明岚) 实测 3~26 秒才吐第一个字, 30 秒能盖住。
 *    要精确就得加"等回复"标志 + 由 on_llm_stream 反通知, 约 20 行, 收益不大。
 *    ⚠️ 已知代价: 若明岚 5 秒就回完, 剩 25 秒仍算忙 → 那一轮天气推迟 1 分钟。
 *      对 5 分钟一次的天气完全无所谓。 */
bool is_busy(void);

/* ================================================================
 * "把识别结果转发给哪个模型" —— 由"选择模型"面板决定
 * ================================================================ */

/*! 切换转发目标。
 *  明岚 = "openclaw" ｜ DeepSeek = "llm"
 *
 *  ★ 只改【目标】, 不立刻发 svc —— 真正的服务切换发生在下次 send_now() 里
 *    (那里先 switch_service + 等 svc_ok, 再发 chat), 所以这里零网络开销,
 *    也不会去抢 weather_poll / voice_task 正在用的 svc 状态。
 *  ★ 若当前有提问正处在流程中 (s_pending), 会拒绝改动:
 *    那时一个 svc_ok 正在路上, 名字对不上就会被当成"别人的"丢掉。
 *  @return true = 已生效 */
bool set_service(const char *service);

/*! 当前转发目标 (只读)。 */
const char *get_service(void);

}  // namespace ask_router
