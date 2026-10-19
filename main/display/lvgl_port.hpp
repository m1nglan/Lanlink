#pragma once

#include <stdint.h>

#include "esp_err.h"

/* ================================================================
 * LVGL 9.5 移植层 (esp_lcd 后端)
 * 职责: lv_init + 320x170 display + 绘图缓冲 + flush 回调 + 2ms tick + LVGL 任务(CPU1)
 *
 * [链] lvgl_port_task(CPU1) → lvgl_port_lock → lv_timer_handler
 *        ├─ 定时器侧: 【ui_timer_cb → UiBridge::drain_queues】(消费队列 + 上屏)
 *        └─ 刷新侧:   lv_refr → 【flush_cb】 → ST7789
 *
 * 线程安全: LVGL 非线程安全 —— 其它任务改 UI 必须 lvgl_port_lock/unlock;
 *   ★★ 但 LVGL 上下文内 (回调 / lv_timer) 绝不能再加锁: 锁非递归, 会自死锁。
 * ================================================================ */

/*! 初始化 LCD + LVGL + 启动 LVGL 任务。app_main 调用, 之后才能 ui_init() */
esp_err_t lvgl_port_init(void);

void lvgl_port_lock(void);      /*!< 取 LVGL 互斥锁 (阻塞) */
void lvgl_port_unlock(void);    /*!< 放 LVGL 互斥锁 */

/*! 注册编码器为 LVGL indev。
 *  ★ 必须持 lvgl_port_lock() 调用 (会建 lv_indev + 默认 group)。
 *  注册后旋转由 lv_timer_handler 内部自动读, 不再需要"从任务里手动注入方向"的入口。 */
esp_err_t lvgl_port_register_encoder_indev(void);

/*! 暂停 / 恢复编码器输入 (false=暂停) —— 录音期间禁止转屏。
 *  ★★ 暂停时【照样取走并清零】编码器累加器: 攒着的话恢复那一刻会一次性倾泻, 连跳几屏。
 *     语义是"暂停期间转的不算数", 不是"先存着待会儿用"。详见 lvgl_encoder_read_cb()。
 *  ★ 只写一个 bool, 任意任务可调, 不用持 LVGL 锁。 */
void lvgl_port_set_encoder_enabled(bool enabled);

/* ================================================================
 * "选择模型"面板的焦点接管
 *
 * ★★ 为什么要单独一套: 平常编码器所在的那个默认 group 里【只有整屏】, 而且被强制
 *    编辑模式 (editing=true) —— 旋转会给屏发 LV_KEY_LEFT/RIGHT, SquareLine 靠它切屏。
 *    面板要的是反过来的东西: 组里两个图标 + 导航模式 (旋转 = 移动焦点, 不发键)。
 *    ⇒ 所以给面板单开一个 group, 进出时把 indev【换绑】过去 / 换回来。
 *
 * ⚠️ 面板模式期间 lvgl_focus_active_screen_locked() 必须短路 (内部有 s_cho_mode 判断),
 *    否则它每 30ms 就把组清回"整屏", 面板焦点立刻失效。
 *
 * ★ 全部只能在 LVGL 上下文 (锁已持有) 调用。
 * ================================================================ */

/*! 进入"聚光灯"模式: indev 换绑到一个临时组, 组里是 a / b 两个对象, 焦点给 focus_obj。
 *
 *  ★ 用途: 模型选择面板 (两个图标) 和 通知面板 (两个消息条目) 都要"编码器上下选 + 点按进"。
 *  ★ 为什么必须换组: 默认组里只有"整屏"且是编辑模式 (旋转发 LV_KEY_LEFT/RIGHT 给屏)。
 *    这里要的是"导航模式 + 组里两个对象" —— 旋转 move focus, 不发键 ✓
 *  ★ 每次 enter 都重建组内容 → 同一个人可以连调用, 不会和上一次的对象串。
 *  @param a / b  组里的两个可聚焦对象 (传 NULL 跳过)
 *  @param focus_obj 初始焦点; NULL 则用组里第一个
 *  ⚠️ 必须配对调用 lvgl_port_cho_exit()。只能在同一 LVGL 上下文调。 */
void lvgl_port_cho_enter(void *a, void *b, void *focus_obj);

/*! 退出面板模式: indev 换回"屏幕组"(恢复切屏)。 */
void lvgl_port_cho_exit(void);

/*! 面板组当前聚焦的对象 (ui_choclaw2 / ui_chodeep2), 没在面板模式时返回 NULL。 */
void *lvgl_port_cho_focused(void);

/*! ★ 指定"当前屏上该拿焦点的对象" (传 NULL = 清除, 回到默认的"整屏拿焦点")。
 *
 *  为什么需要: 编码器旋转时 LVGL 把 LV_KEY_LEFT/RIGHT 发给【聚焦对象】。
 *  默认聚焦的是整屏 → 屏的 SquareLine 回调负责切屏。
 *  但聊天屏要先用这两个键"上下翻聊天记录", 滚到头才切屏 ⇒
 *  得让 chat_view 的 ui_contextPanel 拿到焦点, 它自己在 key 回调里消费/放行。
 *
 *  ★ 设置时机由业务层决定 (chat_view::sync_screen 每轮维护): 在 chat 屏设成
 *    ui_contextPanel, 其它屏设成 NULL。lvgl_port 本身不认识任何具体控件 ✓
 *  ★ 面板模式 (s_cho_mode) 优先级更高 —— 那时本设置被忽略。
 *  ★ 只能在 LVGL 上下文 (锁已持有) 调用。 */
void lvgl_port_set_focus_override(void *obj);

/*! 启动"时间上屏" —— 每 1000ms 刷一次 ui_time3(时:分) / ui_time4(:秒) / ui_date1(日期星期)。
 *  ★ 必须在 ui_init() 之后调用, 且 **必须持 lvgl_port_lock() 调用** ——
 *    内部会 lv_timer_create 并立刻刷一次 label, 都是 LVGL 对象操作。
 *  ★ 回调本身跑在 lv_timer_handler 里, 那时锁已持有 → 回调里【不能】再加锁。
 *  ★ 时间源是系统 RTC (SNTP 在 WiFi 拿到 IP 后自动对时, 见 drivers/wifi.cpp 的 sntp_start_once)。
 *  ★ 未同步时 (年份 < 2020) 把两个 label 清空 —— 免得把 1970 当成当前时间显示。
 *  ★ 值没变就不写 (时:分每分钟才变), 避免白 malloc + 白重绘。 */
esp_err_t lvgl_port_time_start(void);

/*! 启动"开屏门" —— 等 WiFi / 网关握手 / NTP 三项都就绪后, 立刻切到 ui_home。
 *
 *  ★ 替代原来 `ui_main.c` 里"死等 7000ms"的行为: 三项齐了就提前走, 没齐就继续等。
 *  ★ **不改 ui_main.c**: 它排的那次 7 秒切换用 `lv_anim_delete(ui_home, NULL)` 撤销,
 *    所以 SquareLine 重新导出也不会丢这段逻辑。
 *  ★ 兜底: 等满 BOOT_GATE_MAX_WAIT_MS 还没齐就强制切, 免得 WiFi 配置错的机器永远停在开屏。
 *  ★ 必须在 ui_init() 之后、且 **必须持 lvgl_port_lock() 调用** (内部 lv_timer_create)。
 *  ★ 回调跑在 lv_timer_handler 里, 锁已持有 → 回调里不能再加锁。 */
esp_err_t lvgl_port_boot_gate_start(void);
