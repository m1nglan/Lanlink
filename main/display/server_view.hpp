#pragma once

/* ================================================================
 * server_view —— Server 屏: 把 usage_t 写到 5 组控件
 *
 *   ui_cpunum  (label) / ui_cpubar  (bar)   ← cpu %
 *   ui_ramnum  (label) / ui_rambar  (bar)   ← ram_pct %
 *   ui_ramtext (label)                      ← ram_used  ("1.7G/2G")
 *   ui_disknum (label) / ui_diskbar (bar)   ← disk_pct %
 *   ui_dixktext(label)                      ← disk_used ("33.3G/39.0G")
 *   ui_upnum   (label)                      ← tx (上行) KB/s
 *   ui_downnum (label)                      ← rx (下行) KB/s
 *
 * ★ 只认识控件和数据, 不认识 WS/队列 —— 数据由调用方传进来。
 * ★ 全部函数只能在 **LVGL 任务上下文**调用 (drain_queues), 锁已持有。
 * ★ 值没变就不写 (省重排/重绘)。
 * ================================================================ */

#include "business/bus_msg.hpp"

namespace server_view {

/*! 把一份服务器监控写到 Server 屏。 */
void apply(const usage_t *u);

/*! 当前激活屏是不是 Server 屏。
 *  ★ 业务层 (ui_bridge) 用它决定"要不要切 svc usage", 免得认识 ui_Server 这种屏符号。 */
bool is_active(void);

}  // namespace server_view
