#include "business/weather_poll.hpp"

#include <stdio.h>           /* snprintf */

#include "esp_log.h"
#include "esp_timer.h"

#include "business/ask_router.hpp"
#include "business/ui_bridge.hpp"
#include "drivers/ws.hpp"

static const char *TAG = "weather_poll";

/* ------------------ 周期 ------------------ */
/*! 正常周期: 每 5 分钟拉一次 */
#define POLL_PERIOD_US     (5ULL * 60ULL * 1000ULL * 1000ULL)
/*! 忙的时候: 1 分钟后再试 */
#define POLL_RETRY_US      (1ULL * 60ULL * 1000ULL * 1000ULL)
/*! 开机第一次: 稍等一下 —— ws_keeper 刚把连接建起来, 立刻切服务容易撞上握手 */
#define POLL_FIRST_DELAY_US (3ULL * 1000ULL * 1000ULL)

/*! 天气服务名（网关侧） */
#define WEATHER_SERVICE  "weather"

static esp_timer_handle_t s_timer = nullptr;

/*! 现在忙不忙 —— 忙就别抢 svc 状态。 */
static bool busy(void)
{
    if (UiBridge::get().is_asr_active()) {
        return true;                  /* 语音会话进行中 / 刚按下还没切完服务 */
    }
    if (ask_router::is_busy()) {
        return true;                  /* 等 svc_ok / 等 final / 刚发完 chat 的冷却期 */
    }
    return false;
}

static void arm(uint64_t delay_us)
{
    if (s_timer != nullptr) {
        esp_timer_stop(s_timer);      /* 幂等: 没在跑也不会报错 */
        esp_timer_start_once(s_timer, delay_us);
    }
}

/*! 真的去拉一次: 切 svc=weather。
 *  ★ 网关在 svc_ok 之后会【自动推送一次】weather（实测 svc_ok 后约 280ms），
 *    所以这里不用发 {"type":"get"}。见 tools/gw_probe_weather.mjs 的结论。
 *  ★ 也【不切回 text】—— 谁需要别的服务谁自己切:
 *      voice_task 会话开头会切 text, ask_router 会切 openclaw,
 *      所以留在 weather 上是安全的, 而且省一次往返。
 *  ★ timeout=0: 本回调跑在 esp_timer 任务里, 绝不能阻塞。 */
static void do_poll(void)
{
    WS &ws = WS::get();
    if (!ws.is_connected()) {
        ESP_LOGW(TAG, "WS 未连接, 这轮跳过 (1 分钟后再试)");
        arm(POLL_RETRY_US);
        return;
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "{\"type\":\"svc\",\"service\":\"%s\"}", WEATHER_SERVICE);

    ws.set_service(WEATHER_SERVICE);      /* 本地路由记录 (诊断用) */
    esp_err_t err = ws.send_text(msg, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "发 svc %s 失败: %s, 1 分钟后再试",
                 WEATHER_SERVICE, esp_err_to_name(err));
        arm(POLL_RETRY_US);
        return;
    }

    ESP_LOGI(TAG, "已请求天气 (svc %s), 网关会自动推送", WEATHER_SERVICE);
    arm(POLL_PERIOD_US);                  /* 正常周期 */
}

static void poll_cb(void *arg)
{
    (void)arg;

    if (busy()) {
        /* ★ 用户定的规则: 忙就再多等 1 分钟, 还忙就继续等 (不补发、不排队) */
        ESP_LOGI(TAG, "忙 (语音会话或转发进行中), 天气推迟 1 分钟");
        arm(POLL_RETRY_US);
        return;
    }

    do_poll();
}

esp_err_t weather_poll::start(void)
{
    if (s_timer != nullptr) {
        return ESP_OK;                    /* 幂等 */
    }

    const esp_timer_create_args_t args = {
        .callback = &poll_cb,
        .arg      = nullptr,
        .dispatch_method = ESP_TIMER_TASK,   /* 在 esp_timer 任务里跑, 不是 ISR */
        .name     = "weather_poll",
    };
    esp_err_t err = esp_timer_create(&args, &s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 开机第一次: 延迟 3 秒, 避开 WS 刚建连/握手的瞬间 */
    arm(POLL_FIRST_DELAY_US);
    ESP_LOGI(TAG, "天气轮询已启动 (首次 %llus 后, 之后每 %llu 分钟, 忙则 %llu 分钟重试)",
             (unsigned long long)(POLL_FIRST_DELAY_US / 1000000ULL),
             (unsigned long long)(POLL_PERIOD_US / 60000000ULL),
             (unsigned long long)(POLL_RETRY_US / 60000000ULL));
    return ESP_OK;
}
