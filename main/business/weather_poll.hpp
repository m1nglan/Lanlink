#pragma once

/* ================================================================
 * weather_poll —— 天气的定时拉取调度
 *
 * 规则（用户定的）:
 *     开机先拉一次；之后每 5 分钟一次。
 *     到点时如果【忙】→ 不拉，改成 1 分钟后再试；还忙就再等 1 分钟…
 *     一空闲下来立刻拉，然后回到 5 分钟周期。
 *
 * "忙"的判据 = 有一轮语音会话在进行，或者 ask_router 还在等 svc_ok / final / 大模型回复。
 *   ★★ 为什么必须判忙: 网关的 svc 是【全局独占】的 —— 天气要切 svc=weather，
 *      切走会把正在进行的语音会话 / openclaw 回复掐掉（而且不报错，静默失效）。
 *
 * ★ 本模块只做"什么时候发"，不解析、不上屏（那两块在 drivers/weather + display/weather_view）。
 * ★ 用 esp_timer 一次性定时、回调里重新 arm —— 不用任务，也不用 LVGL 上下文。
 *   ★ 回调里只调 WS::send_text(timeout=0) 和读两个 bool，不阻塞。
 * ================================================================ */

#include "esp_err.h"

namespace weather_poll {

/*! 启动天气轮询（立刻拉一次，之后按周期）。
 *  ★ 必须在 WiFi 连上、WS 已 attach 之后调用（main.cpp 里 ws_keeper_start 之后）。 */
esp_err_t start(void);

}  // namespace weather_poll
