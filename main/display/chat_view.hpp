#pragma once

/* ================================================================
 * 聊天视图 —— 气泡的创建 / 更新 / 销毁 + 生命周期状态机
 *
 * ★ 本模块**不认识业务**: 不知道什么是 ASR / LLM / openclaw,
 *   只收"会话开始了 / 结束了 / 我说了这句 / 对方回了这句"。
 * ★ 全部函数只能在 **LVGL 任务上下文**调用 (由 UiBridge::drain_queues 驱动)。
 *   内部不碰队列、不碰锁 —— LVGL 锁由调用方持有。
 * ★ 控件结构 (属性要与 SquareLine 导出一致):
 *     ui_contextpanel(flex column, 可滚) → roll(wrapper) → panel(气泡) → label(文字)
 * ================================================================ */

namespace chat_view {

/*! 每轮调一次 (幂等): 同步屏幕状态 ——
 *  不在聊天屏就清空记录; 聊天屏被重建过就清空旧指针 + 删掉 SquareLine 预置的占位气泡。
 *  @return false = 当前不在聊天屏, 调用者不该继续操作气泡。
 *          (模块内部的上屏函数都会拿它做闸; drain_queues 直接忽略返回值即可) */
bool sync_screen(void);

/*! 会话开始 (resp_q 的 STARTED): 收尾上一条 + 放一个空气泡。 */
void begin_asr(void);

/*! 会话结束 (resp_q 的 ENDED / ABORTED): **只记时刻**, 不立刻收尾
 *  (服务器的 final 是网络回包, 必须留个落点)。真正收尾交给 tick()。 */
void end_asr(void);

/*! 每轮调一次: 宽限期 (CHAT_END_GRACE_MS) 到期就收尾当前气泡。 */
void tick(void);

/*! 上屏: 我说的 (绿底/靠右) / 对方回的 (灰底/靠左)。
 *  ★ text 永远是【完整文本】(V2 协议) → 直接全量替换, 本模块零累积状态。 */
void show_asr(const char *text, bool is_final);
void show_llm(const char *text, bool is_final);

}  // namespace chat_view
