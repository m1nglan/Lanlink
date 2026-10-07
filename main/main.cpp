#include <stdio.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "business/ui_bridge.hpp"
#include "business/voice.hpp"
#include "business/ws_keeper.hpp"
#include "display/lcd_display.hpp"
#include "display/lvgl_port.hpp"
#include "drivers/button_edge.hpp"
#include "drivers/encoder.hpp"
#include "drivers/wifi.hpp"
#include "ui.h"

static const char *TAG = "Main";

/* ================================================================
 * app_main: 屏幕 → 输入 → WiFi → 业务总线 → 业务任务
 *
 * [链] app_main → lvgl_port_init ─┬─ lcd_panel_init → ST7789 + 背光先关
 *                                 └─ lv_init + display + flush_cb + tick + lvgl 任务
 *      → ui_init → 背光开 → encoder_init → indev 注册 → 按键中断 ×2
 *      → WiFi::init (阻塞) → UiBridge::init (建队列) → start_ui_timer
 *      → ws_keeper_start → voice_task_start
 *
 * 自建任务 3 个 (+ 组件自带的 websocket_task): 2026 版已去轮询化
 *   lvgl       CPU1 prio2 6K   lv_timer_handler + 编码器 indev + 消费队列上屏
 *   ws_keeper  CPU0 prio5 8K   WS 连接生命周期: 建连/重连/保活
 *   voice      CPU0 prio6 6K   命令驱动语音会话 (真采音)
 *   websocket  CPU0 prio5 4K   组件自带 (ws.cpp::init 里钉核) —— 回调在此上下文跑
 *
 * 无任务的输入: 按键 = GPIO中断 + esp_timer 消抖 → 投 voice_q;
 *               编码器 = GPIO中断 + 累加器 → LVGL indev 在 lv_timer_handler 里消费
 * 数据流: 按键 → voice_q → voice_task → ASR;  服务器 → resp_q/stream_q → lvgl 上屏
 * ================================================================ */

/* 按键引脚 */
#define BTN_REC_PIN   GPIO_NUM_2   /* IO2: 录音键(按下为低,内部上拉) */
#define BTN_SVC_PIN   GPIO_NUM_42    /* IO42:  服务切换键(按下为低,内部上拉) */

/* ★ 必须静态: WiFi::init() 会把 this 交给 esp_event 永久持有
 *   (esp_event_handler_register(..., this))。若放在 app_main 栈上,
 *   app_main 退出后 WiFi 事件回调就会踩到已释放的栈 → use-after-free。 */
static WiFi s_wifi;

/* 按键边沿回调 —— 跑在 esp_timer 任务上下文。
 * 这里做的全部是"非阻塞投队列", 所以可以安全调用 (8 字节消息, timeout=0)。 */
static void on_button_edge(void *ctx, gpio_num_t pin, bool pressed)
{
    (void)ctx;

    if (pin == BTN_REC_PIN) {
        if (pressed) {
            ESP_LOGI(TAG, "[按键] 录音键 按下 → voice_start");
            UiBridge::get().voice_start();
        } else {
            ESP_LOGI(TAG, "[按键] 录音键 松开 → voice_stop");
            UiBridge::get().voice_stop();
        }
    } else if (pin == BTN_SVC_PIN) {
        /* 服务键: 本阶段只打日志、不投任何队列 (服务切换业务未定)
         * ★ 日志里用 %d 打印真实 GPIO 号 (从 BTN_SVC_PIN 取), 别写死 "IO8" ——
         *   引脚改过一次 (IO8 → GPIO42), 写死的数字会跟着过时, 排查时误导人。 */
        ESP_LOGI(TAG, "[按键] 服务键(IO%d) %s (未接业务)",
                 (int)BTN_SVC_PIN, pressed ? "按下" : "松开");
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "========= Lanlink: 事件驱动调度层 (阶段 0-3 已完成) =========");
    ESP_LOGI(TAG, "空闲堆: %u 字节", (unsigned)esp_get_free_heap_size());

    /* ---- 1. 屏幕: LCD + LVGL 9.5 + SquareLine UI ---- */
    ESP_ERROR_CHECK(lvgl_port_init());
    lvgl_port_lock();
    ui_init();
    lvgl_port_unlock();
    lcd_panel_backlight_on();   /* UI 就绪后再点亮背光 */

    /* ---- 2. 输入: 编码器 indev + 物理按键(中断消抖) ---- */
    ESP_ERROR_CHECK(encoder_init());
    lvgl_port_lock();                                    /* ★ indev 注册须持锁 */
    ESP_ERROR_CHECK(lvgl_port_register_encoder_indev());
    lvgl_port_unlock();
    ESP_ERROR_CHECK(button_edge_init(BTN_REC_PIN, 0, on_button_edge, NULL));
    ESP_ERROR_CHECK(button_edge_init(BTN_SVC_PIN, 0, on_button_edge, NULL));

    /* ---- 3. WiFi (阻塞直到连上或重试耗尽)
     *  ★ 不 ESP_ERROR_CHECK: 没 WiFi 也要能开机(屏幕/按键/编码器照常工作),
     *    WS 连接交给 ws_keeper 持续重试。 */
    esp_err_t wifi_ret = s_wifi.init();
    if (wifi_ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi 未连上 (%s) —— 不中断启动, ws_keeper 会持续重试",
                 esp_err_to_name(wifi_ret));
    }

    /* ---- 4. 业务总线: 建三队列 + asr 绑 WS + 注册结果回调 (不建连) ---- */
    ESP_ERROR_CHECK(UiBridge::get().init());

    /* ---- 5. UI 消费 timer (★ 须持 LVGL 锁; 内部是 lv_timer_create) ---- */
    lvgl_port_lock();
    ESP_ERROR_CHECK(UiBridge::get().start_ui_timer());
    lvgl_port_unlock();

    /* ---- 6. 业务任务 (CPU0; websocket_task 也在 CPU0, 三者共用 client->lock) ---- */
    ESP_ERROR_CHECK(ws_keeper_start());
    ESP_ERROR_CHECK(voice_task_start());

    ESP_LOGI(TAG, "初始化完成: 转编码器切屏 | 按 IO%d 走一轮语音会话 | 按 IO%d 见钩子日志",
             (int)BTN_REC_PIN, (int)BTN_SVC_PIN);
    ESP_LOGI(TAG, "空闲堆(初始化后): %u 字节", (unsigned)esp_get_free_heap_size());

    /* app_main 自身已无用, 删除释放栈 */
    vTaskDelete(NULL);
}
