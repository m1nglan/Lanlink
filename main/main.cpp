#include <stdio.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"             /* 第 0 步: 模型选择的持久化要用 */
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "business/ui_bridge.hpp"
#include "business/voice.hpp"
#include "business/weather_poll.hpp"   /* 天气定时拉取 */
#include "business/ws_keeper.hpp"
#include "display/cho_view.hpp"    /* 选择模型面板: init() 收起来 + 套焦点样式 */
#include "display/msg_ball.hpp"
#include "display/msg_panel.hpp"    /* 主界面的"有新消息"提示球 */
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
/* 按键引脚。
 * ★★ 2026-10 接线调整: 录音键从 GPIO2 迁到 GPIO42（即原来那个"另一个按键"）。
 *    同时把原来在 42 的"服务键"（只打日志、未接业务）挪到 GPIO2 —— 两个键都不丢。
 * ⚠️ 如果以后 GPIO2 不想用了, 把 BTN_SVC_PIN 那一整套 (define + button_edge_init) 删掉即可。
 * ⚠️ GPIO42 是 JTAG 的 MTMS 脚 —— 当普通 GPIO + 中断用没问题
 *    （前提是不接 JTAG 调试）, 原来就一直在这么用。 */
#define BTN_REC_PIN   GPIO_NUM_42  /* IO42: 录音键 (按下为低, 内部上拉) */
#define BTN_SVC_PIN   GPIO_NUM_2   /* IO2:  服务键 (未接业务, 只打日志) */

/* ★ 必须静态: WiFi::init() 会把 this 交给 esp_event 永久持有
 *   (esp_event_handler_register(..., this))。若放在 app_main 栈上,
 *   app_main 退出后 WiFi 事件回调就会踩到已释放的栈 → use-after-free。 */
static WiFi s_wifi;

/* 按键边沿回调 —— 跑在 esp_timer 任务上下文。
 *
 * ★★ 这里【只报告"哪个键按下/松开"】, 不做任何语义判断。
 *    为什么: 同一个键在不同 UI 状态下含义不同 ——
 *      IO42：面板关着 = 录音键 (按下开始/松开结束)
 *            面板开着 = 取消键 (录音禁用)
 *    那种判断要看 LVGL 的当前状态, 只能在 LVGL 任务里做 →
 *    所以按键回调只投一条 RESP_KEY 到 resp_q, 由 drain_queues 决定怎么办。
 *    (语义实现在 ui_bridge.cpp 的 case RESP_KEY) */
static void on_button_edge(void *ctx, gpio_num_t pin, bool pressed)
{
    (void)ctx;

    if (pin == BTN_REC_PIN) {
        UiBridge::get().post_key(KEY_B, pressed);     /* IO42 */
    } else if (pin == BTN_SVC_PIN) {
        UiBridge::get().post_key(KEY_A, pressed);     /* IO2 */
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "========= Lanlink: 事件驱动调度层 (阶段 0-3 已完成) =========");
    ESP_LOGI(TAG, "空闲堆: %u 字节", (unsigned)esp_get_free_heap_size());

    /* ---- 0. NVS: 必须先于任何 nvs_open 的使用者 (cho_view 要读"上次选的模型")。
     *  ★ wifi.cpp 里也会调, 那个是幂等的; 但它的位置在 cho_view::init() 之后 ✗
     *  ❌ ESP_ERR_NVS_NO_FREE_PAGES 说明分区擦过头了, 擦一次重来。 */
    {
        esp_err_t nvs_ret = nvs_flash_init();
        if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            nvs_ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(nvs_ret);
    }

    /* ---- 1. 屏幕: LCD + LVGL 9.5 + SquareLine UI ---- */
    ESP_ERROR_CHECK(lvgl_port_init());
    lvgl_port_lock();
    ui_init();
    /* ★ 选择模型面板: 从 NVS 恢复上次的选择 + 套焦点环样式 + 把面板收起来。
     *   必须在 ui_init() 之后 (控件才存在), 且持锁 (会改样式和高度)。
     *   ⚠️ 它要读 NVS → 所以 nvs_flash_init() 必须在这之前 (见下面第 0 步)。 */
    cho_view::init();
    msg_ball::init();              /* 提示球置成"无未读": messball 隐 / nomessball 显 */
    msg_panel::init();             /* 通知面板收起(宽 0) + 两个子面板隐 + "没有消息~" 显 */
    lvgl_port_unlock();
    lcd_panel_backlight_on();   /* UI 就绪后再点亮背光 */

    /* 时间上屏 + 开屏门: 都必须在 ui_init() 之后 (ui_time3/ui_time4/ui_home 那时才存在)。
     * ★★ 必须持 LVGL 锁调用 —— 它们内部会 lv_timer_create, 而且 time_start 还会
     *    立刻刷一次 ui_time3/ui_time4 (lv_label_set_text 是实打实的对象改动)。
     *    LVGL 非线程安全: 不持锁就从 main 任务改, 会踩着正在跑 lv_timer_handler 的
     *    LVGL 任务 → 内部状态(事件链表/堆)被破坏 → 几十毫秒~几秒后在别处莫名其妙崩。
     *    (这条和下面 start_ui_timer 的规矩是同一条)
     * ★ 时间源靠 WiFi 拿到 IP 后启动的 SNTP (见 drivers/wifi.cpp 的 sntp_start_once),
     *   在同步完成前两个 label 会保持空白。 */
    lvgl_port_lock();
    ESP_ERROR_CHECK(lvgl_port_time_start());
    ESP_ERROR_CHECK(lvgl_port_boot_gate_start());
    lvgl_port_unlock();

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

    /* ---- 7. 天气轮询 (开机一次 + 每 5 分钟; 忙则 1 分钟后重试)
     * ★ 必须放在 ws_keeper_start() 之后 —— 它要 WS 已建连才发得出去。
     * ★ 首次延迟 3 秒, 避开刚握手那一下。
     * ★ 刷屏不是它干的: 它只发 svc weather, 网关自动推 {"type":"weather"},
     *   上屏在 drain_queues 里 (weather_view::apply)。 */
    ESP_ERROR_CHECK(weather_poll::start());

    ESP_LOGI(TAG, "初始化完成: 转编码器切屏 | 按 IO%d 走一轮语音会话 | 按 IO%d 见钩子日志",
             (int)BTN_REC_PIN, (int)BTN_SVC_PIN);
    ESP_LOGI(TAG, "空闲堆(初始化后): %u 字节", (unsigned)esp_get_free_heap_size());

    /* app_main 自身已无用, 删除释放栈 */
    vTaskDelete(NULL);
}
