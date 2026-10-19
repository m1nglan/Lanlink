#pragma once

/* ================================================================
 * msg_panel —— 主界面的"通知展开面板"
 *
 * 结构 (ui_home.c):
 *   ui_messagepanel [314×162, 圆角 19]  ← 宽度 0=收起 / 314=展开
 *   └── ui_messpanel
 *       ├── ui_clawmesspanel [280×49, 默认 HIDDEN] ← 明岚的回复
 *       │   ├── ui_choclaw3   (图标)
 *       │   ├── ui_clawtext   (标题)
 *       │   └── ui_clawanstext (203×18, DOTS 模式)  ← ★ 回复正文
 *       ├── ui_deepmesspanel [280×49, 默认 HIDDEN] ← DeepSeek 的回复
 *       │   ├── ui_chodeep
 *       │   ├── ui_deeptext
 *       │   └── ui_deepanstext                       ← ★ 回复正文
 *       └── ui_nomessagetext [282×53, "没有消息~"]   ← 无通知时显示
 *
 * 交互:
 *   · 新消息到达 → 按"当前服务"把正文实时写进 clawanstext / deepanstext,
 *     并显示对应的子面板、隐藏"没有消息~"
 *   · home 页按 IO2 → openmessage_Animation 展开 (宽度 0 → 314)
 *   · 展开时编码器在两个子面板间移动焦点, 点按进入 chat 屏
 *   · 进 chat 屏 (不管是点按进的还是转编码器进的) → 收起面板 + 清正文 +
 *     两个子面板隐藏 + 显示"没有消息~"
 *
 * ★ 本模块只管面板和文字, 不认识 WS / 队列。
 * ★ 全部函数只能在 **LVGL 任务上下文**调用 (drain_queues), 锁已持有。
 * ================================================================ */

namespace msg_panel {

/*! 两个通知条目 —— 下标对应 selected()/focused_entry() 的返回值 */
enum entry_t {
    ENTRY_CLAW = 0,    /*!< 明岚 / openclaw */
    ENTRY_DEEP,        /*!< DeepSeek / llm */
};

/*! 开机调一次: 面板宽度归 0 (收起) + 两个子面板隐藏 + 显示"没有消息~"。 */
void init(void);

/*! 面板是否展开 */
bool is_open(void);

/*! 面板里【有没有真消息】(clawmesspanel / deepmesspanel 至少一个可见)。
 *  ★ 展开但没有消息时只显示"没有消息~" —— 那种情况点按 IO2 应该只是【收起】,
 *    不该进聊天页 ✗ (踩过: 空面板点一下直接跳 chat) */
bool has_entry(void);

/*! 现在能不能展开 (当前激活屏是 home 屏)。
 *  ★ 和 cho_view::can_open 一个套路: "哪个屏允许开"是 UI 层的知识。 */
bool can_open(void);

/*! 展开面板 (openmessage_Animation: 宽度 0 → 314, 500ms) 并接管编码器。 */
void open(void);

/*! 收起面板 (宽度直接归 0, 不播动画) 并交还编码器。 */
void close(void);

/*! 新消息正文 (流式) —— 实时替换。
 *  @param text    正文 (每条 partial 都调, 全量替换)
 *  @param is_claw true = 写进明岚那条 (clawanstext), false = DeepSeek 那条 (deepanstext)
 *  ★ 由调用方 (ui_bridge) 用 ask_router::get_service() 判断该走哪条 ——
 *    display 层不反向依赖 business 层 ✓
 *  ★ 内容没变就不写 (省一次 label 重排)。 */
void set_reply_text(const char *text, bool is_claw);

/*! 编码器当前聚焦的条目 (面板展开时有效); 面板没开返回上次写入的那一条。 */
int focused_entry(void);

/*! 进入聊天屏 (MOVE_RIGHT 动画, 500ms)。
 *  ★ 为什么放在这里而不是调用方: 切屏要 ui_chat / _ui_screen_change 这些
 *    SquareLine 符号 —— 业务层 (ui_bridge) 不该认识它们 ✓ */
void enter_chat(void);

/*! 标记"已读": 收起面板 + 清空正文 + 两个子面板隐藏 + 显示"没有消息~"。
 *  ★ 进入 chat 屏时调 (不管怎么进的)。★ 只在 chat 屏生效, 其它屏直接 return。 */
void mark_read(void);

}  // namespace msg_panel
