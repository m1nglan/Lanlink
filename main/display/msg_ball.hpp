#pragma once

/* ================================================================
 * msg_ball —— 主界面上的"有新消息"提示球
 *
 * 结构 (ui_home.c):
 *   ui_messballpanel (圆角胶囊, flex ROW)
 *   ├── ui_messball      [24×24, 默认 HIDDEN]  ← 有未读时显示 (message.png 图标)
 *   ├── ui_balball       (模板 2, 暂不接)
 *   ├── ui_serball       (模板 3, 暂不接)
 *   └── ui_nomessball    [默认可见]            ← 无未读时显示 ("没有消息")
 *
 * 交互:
 *   · 收到 LLM / openclaw 的**完整回复**且当时【不在 chat 屏】
 *       → 隐性 nomessball / 显示 messball  (只在第一次置位, 不重复操作)
 *   · 之后切到 chat 屏
 *       → 隐性 messball / 显示 nomessball  (表示消息已读)
 *
 * ★ 两个对象都是 SquareLine 建好的, 这里**只切 HIDDEN 标志**, 不创建/不销毁任何东西
 *   —— "不要重复创建 只一个就行" 自然满足。
 * ★ 全部函数只能在 **LVGL 任务上下文**调用 (drain_queues), 锁已持有。
 * ================================================================ */

namespace msg_ball {

/*! 开机调一次: 置成"无未读"状态 (messball 隐 / nomessball 显)。
 *  ★ 显式设置而不依赖 SquareLine 的导出默认值 —— 重导一次就可能变。 */
void init(void);

/*! 来了一个新消息 (LLM/openclaw 的完整回复)。
 *  当前在 chat 屏 → 什么都不做 (用户正看着呢);
 *  已经在未读状态 → 什么都不做 (幂等, 不重复触发)。 */
void notify_incoming(void);

/*! 每轮调一次 (drain_queues): 如果在未读状态且用户已经切到 chat 屏 → 标记已读。 */
void update(void);

}  // namespace msg_ball
