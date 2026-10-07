#include "drivers/encoder.hpp"
#include "drivers/gpio_isr_once.hpp"

#include <stdint.h>

#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "enc";

/* A/B 相状态跳变方向表。
 * 状态编码: bit1=A, bit0=B; 索引 = (上一状态<<2)|当前状态。
 * 合法跳变给 ±1(顺/逆时针), 抖动/非法跳变给 0 忽略。 */
static const int8_t s_dir_tbl[16] = {
     /* 00  01  10  11 */
        0, -1,  1,  0,   /* 上一状态 00 */
        1,  0,  0, -1,   /* 上一状态 01 */
       -1,  0,  0,  1,   /* 上一状态 10 */
        0,  1, -1,  0    /* 上一状态 11 */
};

static volatile uint8_t s_last = 0;       /* 最近一次两相电平 (仅 ISR 读写) */
static int32_t          s_accum = 0;      /* 净跳变累加器 (临界区保护) */

/* 临界区锁: ISR 用 _ISR 变体, 任务用普通变体, 保证跨核互斥 (ESP-IDF 标准做法) */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* [链] GPIO中断(A/B双相) → 【encoder_isr】 → s_accum → [lvgl任务] lvgl_encoder_read_cb
 * 任一编码器脚跳变都进来, 查方向表解出 ±1 并累加。
 * 抖动引起的来回(+1/-1)会在累加器里自然抵消。 */
static void IRAM_ATTR encoder_isr(void *arg)
{
    (void)arg;
    uint8_t now = (uint8_t)(((uint32_t)gpio_get_level(ENC_PIN_A) << 1) |
                            (uint32_t)gpio_get_level(ENC_PIN_B));
    int8_t d = s_dir_tbl[(uint8_t)((s_last << 2) | now)];
    s_last = now;
    if (d != 0) {
        portENTER_CRITICAL_ISR(&s_mux);
        s_accum += d;
        portEXIT_CRITICAL_ISR(&s_mux);
    }
}

esp_err_t encoder_init(void)
{
    /* 1. GPIO 中断服务: 与 button_edge 共用, 幂等(见 gpio_isr_once.hpp) */
    esp_err_t ret = gpio_isr_service_ensure();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_service_ensure: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 2. A/B 相: 输入 + 上拉 + 双边沿中断 */
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << ENC_PIN_A) | (1ULL << ENC_PIN_B);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_ANYEDGE;
    ret = gpio_config(&io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 3. 当前两相电平作为状态机初值 */
    s_last = (uint8_t)(((uint32_t)gpio_get_level(ENC_PIN_A) << 1) |
                       (uint32_t)gpio_get_level(ENC_PIN_B));
    portENTER_CRITICAL(&s_mux);
    s_accum = 0;
    portEXIT_CRITICAL(&s_mux);

    /* 4. 挂中断 (无任务: 消费端在 LVGL 的 indev read_cb 里, 见 lvgl_port) */
    gpio_isr_handler_add(ENC_PIN_A, encoder_isr, NULL);
    gpio_isr_handler_add(ENC_PIN_B, encoder_isr, NULL);

    ESP_LOGI(TAG, "encoder init: A=%d B=%d key_step=%d (无任务, ISR+累加器)",
             ENC_PIN_A, ENC_PIN_B, ENC_KEY_STEP);
    return ESP_OK;
}

int encoder_consume_raw(void)
{
    /* [链同] 消费端: 临界区取走全部净跳变并清零 —— 跨核安全, 不丢 ISR 期间的累加 */
    portENTER_CRITICAL(&s_mux);
    int32_t v = s_accum;
    s_accum = 0;
    portEXIT_CRITICAL(&s_mux);
    return (int)v;
}
