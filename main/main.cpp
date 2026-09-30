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
 * 阶段 2: WiFi + WS 长连接 + 事件驱动调度层
 *
 * 自建任务一共 3 个 (对比重构前的 4 个 + 状态机轮询):
 *   lvgl       CPU1 prio2 6K   渲染 + 编码器 indev + UI 队列消费
 *   ws_keeper  CPU0 prio5 8K   WS 连接生命周期: 建连/重连/保活/切 text
 *   voice      CPU0 prio6 6K   命令驱动语音会话 (本阶段为假会话, 不采音)
 *   ── 另有组件自带的 websocket_task (CPU0 prio5, 在 ws.cpp::init 里钉核)
 *
 * 输入 (无任务):
 *   按键   → GPIO 中断 + esp_timer 消抖 → 回调里投 voice_q
 *   编码器 → GPIO 中断 + 累加器 → LVGL indev 在自己的 timer 里消费
 *
 * 数据流 (跨任务全部走队列; 发送则是同步函数调用):
 *   按键 → voice_q → voice_task → asr.start/send_audio/end ──> 服务器
 *   服务器 ──> websocket_task(回调) → resp_q/stream_q → lvgl 任务上屏
 * ================================================================ */

/* 按键引脚 */
#define BTN_REC_PIN   GPIO_NUM_10   /* IO10: 录音键(按下为低,内部上拉) */
#define BTN_SVC_PIN   GPIO_NUM_8    /* IO8:  服务切换键(按下为低,内部上拉) */

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
        /* IO8: 本阶段只打日志、不投任何队列 (服务切换业务未定) */
        ESP_LOGI(TAG, "[按键] 服务键 %s (IO8 未接业务)", pressed ? "按下" : "松开");
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "========= Lanlink 阶段2: 事件驱动调度层 =========");
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

    ESP_LOGI(TAG, "初始化完成: 转编码器切屏 | 按 IO10 走一轮语音会话 | 按 IO8 见钩子日志");
    ESP_LOGI(TAG, "空闲堆(初始化后): %u 字节", (unsigned)esp_get_free_heap_size());

    /* app_main 自身已无用, 删除释放栈 */
    vTaskDelete(NULL);
}
