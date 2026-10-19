#pragma once

/* ================================================================
 * weather_view —— 把 weather::info_t 写到主界面的三个控件
 *
 *   ui_weather1        图片   ← kind (1~8) 决定用哪张
 *   ui_degree1         数字   ← temp (只写数字, ℃ 由旁边的 ui_degreeimage1 图标表示)
 *   ui_Weather_kind1   文字   ← text
 *
 * ★ 本模块**只认识控件和图片**, 不认识 WS/队列 —— 数据由调用方传进来。
 * ★ 全部函数只能在 **LVGL 任务上下文**调用 (drain_queues), 锁由调用方持有。
 * ================================================================ */

#include "drivers/weather.hpp"

namespace weather_view {

/*! 把一份天气写到屏幕。值没变的部分不会重写 (避免白 malloc + 白重绘)。 */
void apply(const weather::info_t *w);

}  // namespace weather_view
