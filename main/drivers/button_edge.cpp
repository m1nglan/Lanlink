#include "drivers/button_edge.hpp"

#include <string.h>

#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "btn_edge";

/* 每个按键的上下文 (静态池, 最多 BTN_EDGE_MAX_BUTTONS 个) */
typedef struct {
    gpio_num_t          pin;
    int                 active_level;
    bool                last_settled;   /*!< 上次已上报的稳定状态 */
    btn_edge_cb_t       cb;
    void               *ctx;
    esp_timer_handle_t  timer;          /*!< 一次性消抖 timer (ISR 里 restart) */
} btn_edge_t;

static btn_edge_t s_btns[BTN_EDGE_MAX_BUTTONS];
static int        s_btn_count = 0;

/* 诊断计数 (稳定后可删) */
static volatile uint32_t s_isr_hits   = 0;   /* ISR 被触发次数 */
static volatile uint32_t s_timer_hits = 0;   /* 消抖 timer 到期次数 */

/* 中断: 任意边沿都进来, 只重启消抖 timer (推迟上报, 直到电平稳定)。
 * 注意: esp_timer_restart 对"未启动"的 timer 会返回 INVALID_STATE 且不启动它,
 * 故首次边沿必须用 start_once 启动; 之后 restart 推迟。
 * 两者都 IRAM 安全(内部 portENTER_CRITICAL_SAFE), 可在 ISR 调用。 */
static void IRAM_ATTR button_edge_isr(void *arg)
{
    btn_edge_t *b = (btn_edge_t *)arg;
    s_isr_hits++;
    const uint64_t debounce_us = (uint64_t)BTN_EDGE_DEBOUNCE_MS * 1000;
    if (esp_timer_restart(b->timer, debounce_us) != ESP_OK) {
        esp_timer_start_once(b->timer, debounce_us);   /* 尚未启动 → 启动它 */
    }
}

/* timer 到期: 电平已稳定, 读电平并上报 (仅在状态变化时) */
static void button_edge_timer_cb(void *arg)
{
    btn_edge_t *b = (btn_edge_t *)arg;
    s_timer_hits++;
    bool pressed = (gpio_get_level(b->pin) == b->active_level);

    if (pressed != b->last_settled) {
        b->last_settled = pressed;
        ESP_LOGI(TAG, "GPIO%d %s", b->pin, pressed ? "按下" : "释放");
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

    /* 1. GPIO 中断服务 (encoder 可能已装, 容忍 INVALID_STATE) */
    esp_err_t ret = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service: %s", esp_err_to_name(ret));
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

    /* 3. 记录初始稳定状态 (避免上电首帧误报边沿) */
    b->last_settled = (gpio_get_level(pin) == active_level);

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

/* ---- 诊断接口 (排查用, 稳定后可删) ---- */

uint32_t button_edge_isr_hits(void)
{
    return s_isr_hits;
}

uint32_t button_edge_timer_hits(void)
{
    return s_timer_hits;
}
