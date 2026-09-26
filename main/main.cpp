#include <stdio.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "drivers/button_edge.hpp"
#include "drivers/encoder.hpp"
#include "display/lvgl_port.hpp"
#include "display/lcd_display.hpp"
#include "ui.h"

static const char *TAG = "Main";

/* ================================================================
 * 阶段 1 最小验证版: LVGL + 编码器(indev) + 物理按键(中断消抖)
 *
 * 目的: 验证"输入事件化"两条链路
 *   1. 编码器 → LVGL indev → 切屏 (无独立任务, 无跨任务锁)
 *   2. 按键 → GPIO中断+esp_timer消抖 → 回调 (无轮询)
 *
 * 本阶段不接: WiFi / WS / 语音 / LLM / 状态机 (后续阶段再加)
 * ================================================================ */

/* 按键引脚 */
#define BTN_REC_PIN   GPIO_NUM_10   /* IO10: 录音键(按下为低,内部上拉) */
#define BTN_SVC_PIN   GPIO_NUM_8    /* IO8:  服务切换键(按下为低,内部上拉) */

/* 按键边沿回调 (esp_timer 任务上下文, 可安全投队列/调非阻塞 API)
 * 本阶段只打日志; 后续阶段在这里投 cmd_queue 触发 voice_start/stop。 */
static void on_button_edge(void *ctx, gpio_num_t pin, bool pressed)
{
    (void)ctx;
    if (pin == BTN_REC_PIN) {
        ESP_LOGI(TAG, "[按键] 录音键 %s", pressed ? "按下 (未来→voice_start)" : "松开 (未来→voice_stop)");
    } else if (pin == BTN_SVC_PIN) {
        ESP_LOGI(TAG, "[按键] 服务键 %s", pressed ? "按下 (未来→切服务)" : "松开");
    }
}


extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "========= Lanlink 阶段1: 输入事件化 =========");
    ESP_LOGI(TAG, "空闲堆: %u 字节", (unsigned)esp_get_free_heap_size());

    /* ---- 1. 屏幕: LCD + LVGL 9.5 + SquareLine UI ---- */
    ESP_ERROR_CHECK(lvgl_port_init());
    lvgl_port_lock();
    ui_init();
    lvgl_port_unlock();
    lcd_panel_backlight_on();   /* UI 就绪后再点亮背光 */

    /* ---- 2. 编码器: GPIO+ISR(无任务) + 注册为 LVGL indev ---- */
    ESP_ERROR_CHECK(encoder_init());
    lvgl_port_lock();                                    /* ★ indev 注册须持锁 */
    ESP_ERROR_CHECK(lvgl_port_register_encoder_indev());
    lvgl_port_unlock();

    /* ---- 3. 物理按键: 中断 + esp_timer 消抖 (无轮询) ---- */
    ESP_ERROR_CHECK(button_edge_init(BTN_REC_PIN, 0, on_button_edge, NULL));
    ESP_ERROR_CHECK(button_edge_init(BTN_SVC_PIN, 0, on_button_edge, NULL));

    ESP_LOGI(TAG, "初始化完成: 转编码器应能切屏; 按 IO10/IO8 应见按键日志");

    /* app_main 收尾: 任务创建后自身无用,删除释放栈 */
    vTaskDelete(NULL);
}
