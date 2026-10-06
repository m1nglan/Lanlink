#include "drivers/button_edge.hpp"
#include "drivers/gpio_isr_once.hpp"

#include <string.h>

#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"     /* vTaskDelay / xTaskGetTickCountFromISR */

static const char *TAG = "btn_edge";

/* 每个按键的上下文 (静态池, 最多 BTN_EDGE_MAX_BUTTONS 个) */
typedef struct {
    gpio_num_t          pin;
    int                 active_level;
    bool                last_settled;   /*!< 上次已上报的稳定状态 */
    btn_edge_cb_t       cb;
    void               *ctx;
    esp_timer_handle_t  timer;          /*!< 一次性消抖 timer (ISR 里 restart) */
    uint32_t            last_edge_tick; /*!< ISR 限流: 上次进 ISR 的 tick */
    uint32_t            isr_at_report;  /*!< 上次上报时的 ISR 计数 (诊断) */
} btn_edge_t;

static btn_edge_t s_btns[BTN_EDGE_MAX_BUTTONS];
static int        s_btn_count = 0;

/* GPIO 边沿中断累计次数 (诊断用, 见 button_edge_isr_count) */
static volatile uint32_t s_isr_cnt = 0;

uint32_t button_edge_isr_count(void)
{
    return s_isr_cnt;
}

/* 中断: 任意边沿都进来, 只重启消抖 timer (推迟上报, 直到电平稳定)。
 * 注意: esp_timer_restart 对"未启动"的 timer 会返回 INVALID_STATE 且不启动它,
 * 故首次边沿必须用 start_once 启动; 之后 restart 推迟。
 * 两者都 IRAM 安全(内部 portENTER_CRITICAL_SAFE), 可在 ISR 调用。 */
static void IRAM_ATTR button_edge_isr(void *arg)
{
    btn_edge_t *b = (btn_edge_t *)arg;

    s_isr_cnt++;                            /* 诊断计数 (ISR 里只做 ++, 很便宜) */

    /* ★★ 限流: 同一个 tick(10ms @HZ=100) 内只做一次。
     *
     *   esp_timer_restart 从 ISR 里调【安全但不轻】—— 读 esp_timer.c:135-177:
     *     timer_list_lock()                 → portENTER_CRITICAL_SAFE (屏蔽中断)
     *     esp_timer_impl_get_time()         → 读 systimer + 64 位乘除
     *     timer_remove()                    → ★ 从有序链表里摘出来 (遍历)
     *     timer_insert()                    → ★ 再遍历一次找位置插回去
     *     timer_list_unlock()               → 恢复中断
     *
     *   按键抖动/引脚噪声时 ISR 会被反复重入, 每次都要屏蔽一遍中断 →
     *   CPU0 几乎 100% 泡在这个 ISR 里 → 中断看门狗(300ms)永远轮不上 →
     *   "Interrupt wdt timeout on CPU0"。
     *   (实测 backtrace 就停在本函数里: button_edge_isr → esp_timer_restart。)
     *
     *   限流后最多 100 次/秒, 对 50ms 消抖完全够用。
     *   xTaskGetTickCountFromISR 只是读个变量, 比 esp_timer_restart 便宜几个数量级。 */
    uint32_t now = xTaskGetTickCountFromISR();
    if (now == b->last_edge_tick) {
        return;
    }
    b->last_edge_tick = now;

    const uint64_t debounce_us = (uint64_t)BTN_EDGE_DEBOUNCE_MS * 1000;
    if (esp_timer_restart(b->timer, debounce_us) != ESP_OK) {
        esp_timer_start_once(b->timer, debounce_us);   /* 尚未启动 → 启动它 */
    }
}

/* timer 到期: 电平已稳定, 读电平并上报 (仅在状态变化时) */
static void button_edge_timer_cb(void *arg)
{
    btn_edge_t *b = (btn_edge_t *)arg;
    bool pressed = (gpio_get_level(b->pin) == b->active_level);

    if (pressed != b->last_settled) {
        b->last_settled = pressed;

        /* ★ 把"距上次上报期间来了多少次中断"一起打出来 —— 一眼看出有没有抖动风暴:
         *   正常一次按键 = 几次~几十次; 几百/几千 = 引脚在噪声里翻转
         *   (接触不良/悬空/上拉没接上), 会引发 ISR 风暴 → 中断看门狗 panic。 */
        uint32_t isr_now = s_isr_cnt;
        ESP_LOGI(TAG, "GPIO%d %s (期间中断 %u 次, 累计 %u)",
                 b->pin, pressed ? "按下" : "释放",
                 (unsigned)(isr_now - b->isr_at_report), (unsigned)isr_now);
        b->isr_at_report = isr_now;

        if (b->cb != NULL) {
            b->cb(b->ctx, b->pin, pressed);   /* 普通任务上下文, 可安全投队列 */
        }
    }
}

esp_err_t button_edge_init(gpio_num_t pin, int active_level, btn_edge_cb_t cb, void *ctx)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_btn_count >= BTN_EDGE_MAX_BUTTONS) {
        ESP_LOGE(TAG, "按键数超过上限 %d", BTN_EDGE_MAX_BUTTONS);
        return ESP_ERR_NO_MEM;
    }

    /* 1. GPIO 中断服务: 与 encoder 共用, 幂等(见 gpio_isr_once.hpp) */
    esp_err_t ret = gpio_isr_service_ensure();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_service_ensure: %s", esp_err_to_name(ret));
        return ret;
    }

    btn_edge_t *b = &s_btns[s_btn_count];
    memset(b, 0, sizeof(*b));
    b->pin = pin;
    b->active_level = active_level;
    b->cb = cb;
    b->ctx = ctx;

    /* 2. GPIO: 输入 + 上拉(active=0 时) + 双边沿中断 */
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << pin);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = (active_level == 0) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    io.pull_down_en = (active_level == 0) ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
    io.intr_type = GPIO_INTR_ANYEDGE;
    ret = gpio_config(&io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 3. 记录初始稳定状态 (避免上电首帧误报边沿)
     *
     * ★★ 不能刚 gpio_config 就立刻采初值:
     *   上拉刚使能 / 面包板接触 / 外部噪声 —— 引脚那一刻可能还没稳。
     *   此刻读到低 → last_settled 被记成"按下" → 等它稳下来触发一次边沿,
     *   消抖后对比发现变了 → 【误报一条"释放"】。
     *   实测踩过: 开机 2.4 秒、用户没碰按键, 冒出 "[按键] 录音键 松开 → voice_stop"
     *   (而这个假"松开"还会把 voice_q 里的 VOICE_STOP 语义搅乱)。
     *
     *   连采到"连续 10 次不变"为止 (最多 100ms) 再定初值。 */
    int stable = gpio_get_level(pin);
    for (int i = 0; i < 10; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
        int lvl = gpio_get_level(pin);
        if (lvl != stable) {
            stable = lvl;
            i = 0;                  /* 变了就重数, 要求连续 10 次稳定 */
        }
    }
    b->last_settled = (stable == active_level);
    ESP_LOGI(TAG, "GPIO%d 初始电平稳定在 %d (active_level=%d)",
             pin, stable, active_level);

    /* 4. 一次性消抖 timer (初建为停止态, 首次 ISR restart 才启动) */
    esp_timer_create_args_t targs = {};
    targs.callback = button_edge_timer_cb;
    targs.arg = b;
    targs.dispatch_method = ESP_TIMER_TASK;
    targs.name = "btn_edge";
    ret = esp_timer_create(&targs, &b->timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 5. 挂中断 */
    ret = gpio_isr_handler_add(pin, button_edge_isr, b);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add: %s", esp_err_to_name(ret));
        return ret;
    }

    s_btn_count++;
    ESP_LOGI(TAG, "GPIO%d init OK (active=%d, 消抖=%dms)",
             pin, active_level, BTN_EDGE_DEBOUNCE_MS);
    return ESP_OK;
}
