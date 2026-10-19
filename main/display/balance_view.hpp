#pragma once

/* ================================================================
 * balance_view —— Balance 屏: 显示余额
 *
 *   ui_balancenum (label, lv_font_montserrat_38) ← "11.96"
 *   ui_balicon    (label "¥")                     ← 静态, 不用管
 *   ui_baltext    (label "余额")                  ← 静态
 *
 * ★ 网关侧行为 (新加的 API 文档):
 *   切到 svc=balance → 连着回 {"type":"svc_ok"} + {"type":"balance","total":11.96,...}
 *   之后【不会定时推】✗ 想刷新就发 {"type":"get"}
 *   切走再切回来会重新推一次 ✓
 *   ⇒ 所以和 Server 屏一样用"停留才切"的逻辑: 停在 Balance 屏 1 秒 → 切 svc balance
 *     → 网关推一次 → 显示 ✓ 离开时切回 text。
 *
 * ★ 全部函数只能在 **LVGL 任务上下文**调用 (drain_queues), 锁已持有。
 * ================================================================ */

namespace balance_view {

/*! 把余额写到 ui_balancenum。值没变就不写。
 *  @param total 余额(元)。格式化用 "%.2f" → 例如 11.96 */
void apply(float total);

/*! 当前激活屏是不是 Balance 屏。
 *  ★ 业务层 (ui_bridge) 用它决定"要不要切 svc balance", 免得认识 ui_Balance 这种屏符号。 */
bool is_active(void);

/*! 进入"刷新中"状态: 标签先显示 "--.--", 等网关回 balance 再写真实数字。
 *  ★★ 必须同时把内部的"上次值"缓存作废 ——
 *    否则余额没变时 apply() 会判定"没变化"直接 return, 标签就永远停在 "--.--" ✗
 *  ★ 只在 LVGL 上下文 (锁已持有) 调用。 */
void begin_refresh(void);

}  // namespace balance_view
