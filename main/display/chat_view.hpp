#pragma once

/* ================================================================
 * 聊天视图 —— 气泡的创建 / 更新 / 销毁 + 生命周期状态机
 *
 * ★ 本模块**不认识业务**: 不知道什么是 ASR / LLM / openclaw,
 *   只收"会话开始了 / 结束了 / 我说了这句 / 对方回了这句"。
 * ★ 全部函数只能在 **LVGL 任务上下文**调用 (由 UiBridge::drain_queues 驱动)。
 *   内部不碰队列、不碰锁 —— LVGL 锁由调用方持有。
 * ★ 控件结构 (属性要与 SquareLine 导出一致):
 *     ui_contextPanel(flex column, 可滚) → roll(wrapper) → panel(气泡) → label(文字)
 *   ⚠️ 名字是 SquareLine 导出给的: 大驼峰 `contextPanel` / 小写 `resrollpanel`
 *      (不是全小写的 contextpanel / resrollpane —— 改 UI 时如果又改名, 这里要跟着改)
 * ================================================================ */

namespace chat_view {

/*! 每轮调一次 (幂等): 同步屏幕状态 ——
 *  面板不存在就清空记录; 首次进屏时清空旧指针 + 删掉 SquareLine 预置的占位气泡。
 *  @return false = 拿不到 ui_contextPanel (没有可用的父对象), 调用者不该继续操作气泡。
 *          ★ 注意返回值【不代表"在不在聊天屏"】: 新版导出在 ui_init() 里就把聊天屏
 *            建好了 (ui.c:549) 且永不销毁, 所以面板从开机起就存在 —— 见 sync_screen 的说明。
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

/*! 清空整个聊天面板 (删掉所有气泡 + 清 PSRAM 历史缓冲)。
 *  ★ 切换模型时用 —— 屏幕上的旧气泡留着只会误导。
 *  ★ 只清【屏幕】, 不发网关的 {"type":"clear"}: 网关的服务上下文是按服务
 *    各存一份的, 切到 llm / openclaw 本身就换了一套上下文, 不需要额外清。
 *  ★ 必须在 LVGL 上下文 (锁已持有) 调用。 */
void clear_all(void);

}  // namespace chat_view
