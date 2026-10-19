#pragma once

/* ================================================================
 * cho_view —— chat 屏上的"选择模型"面板
 *
 *   ui_chat
 *   └── ui_choPanel            (320 × 100, 深灰 0x494850, 圆角 10)
 *       ├── ui_chotext2        "选择模型"
 *       └── ui_choinPanel2     (304 × 62, flex ROW)
 *           ├── ui_choclaw2    48×48 企鹅  (明岚 / openclaw)
 *           └── ui_chodeep2    48×48 logo  (DeepSeek)
 *
 * 交互（用户定的）:
 *   · chat 屏上按 KEY_A(IO2)  → 展开面板, 编码器旋转在两项间移动焦点
 *   · 面板开着时按 KEY_A      → 确认 (记下选择 + 收起 + 通知业务)
 *   · 面板开着时按 KEY_B(IO42) → 取消 (只收起, 不改选择)
 *   · 面板开着时【录音禁用】; 面板关着时 KEY_B 就是录音键
 *
 * ★ 本模块只管"面板本身"(开/合/焦点/选中项), **不决定按键语义** ——
 *   语义在 UiBridge::drain_queues 的 case RESP_KEY 里, 因为那里同时能看到
 *   面板状态和语音状态。
 * ★ 全部函数只能在 **LVGL 任务上下文**调用 (drain_queues), 锁已持有。
 * ================================================================ */

namespace cho_view {

/*! 两个可选项 —— 下标就是 selected() 的返回值 */
enum choice_t {
    CHOICE_CLAW = 0,    /*!< 明岚 / openclaw (企鹅图标) */
    CHOICE_DEEP,        /*!< DeepSeek */
};

/*! 开机调一次: 套焦点样式 + 把面板收起来 (SquareLine 导出时它是展开的)。
 *  ★ 必须在 ui_init() 之后、且持 LVGL 锁。 */
void init(void);

/*! 面板是否展开 */
bool is_open(void);

/*! 现在能不能打开面板 (当前激活屏是 chat 屏)。
 *  ★ 为什么这条判断放在这里而不是调用方: "哪个屏允许开面板"是 UI 层的事,
 *    UiBridge 不该认识 ui_chat 这种屏符号。 */
bool can_open(void);

/*! 展开面板 (隐含: 焦点交给第一项, 编码器改为"移动焦点"模式) */
void open(void);

/*! 收起面板 (焦点/编码器交还给屏幕, 恢复切屏功能) */
void close(void);

/*! 当前选中项 (CHOICE_CLAW / CHOICE_DEEP)。面板没开时返回上次确认的值。 */
int selected(void);

/*! 面板开着时, 从编码器焦点反推选中项 (用户转到了哪个图标)。
 *  面板没开 → 等同于 selected()。给"确认"那一刻用。 */
int focused_choice(void);

/*! 直接设置选中项 (第 2 步接业务时用, 比如开机从 NVS 恢复上次选择) */
void set_selected(int choice);

}  // namespace cho_view
