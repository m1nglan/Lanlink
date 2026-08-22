#include "drivers/encoder.hpp"

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_attr.h"

static const char *TAG = "enc";

/* 事件队列 + 用户回调 */
static QueueHandle_t s_queue = NULL;
static encoder_cb_t  s_cb   = NULL;

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

static volatile uint8_t s_last = 0;   /* 最近一次两相电平 */

/* 双边沿中断: 任一编码器脚跳变都进来, 状态机解出旋转方向 */
static void IRAM_ATTR encoder_isr(void *arg)
{
    (void)arg;
    uint8_t now = (uint8_t)(((uint32_t)gpio_get_level(ENC_PIN_A) << 1) |
                            (uint32_t)gpio_get_level(ENC_PIN_B));
    int8_t d = s_dir_tbl[(uint8_t)((s_last << 2) | now)];
    s_last = now;
    if (d == 0) {
        return;
    }
    encoder_dir_t evt = (d > 0) ? ENC_DIR_RIGHT : ENC_DIR_LEFT;
    BaseType_t hpw = pdFALSE;
    xQueueSendFromISR(s_queue, &evt, &hpw);
    if (hpw == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* 驱动任务: 累计状态跳变, 每满 ENC_KEY_STEP(约一格 detent) 触发一次回调。
 * 抖动引起的来回(+1/-1)会相互抵消, 不会误触发。 */
static void encoder_task(void *arg)
{
    (void)arg;
    encoder_dir_t evt;
    int32_t acc = 0;
    while (1) {
        if (xQueueReceive(s_queue, &evt, pdMS_TO_TICKS(50)) == pdTRUE) {
            acc += (evt == ENC_DIR_RIGHT) ? 1 : -1;
            if (acc >= ENC_KEY_STEP || acc <= -ENC_KEY_STEP) {
                encoder_dir_t out = (acc > 0) ? ENC_DIR_RIGHT : ENC_DIR_LEFT;
                if (s_cb != NULL) {
                    s_cb(out);
                }
                acc = 0;
            }
        }
    }
}

esp_err_t encoder_init(void)
{
    /* 1. GPIO 中断服务 (项目其他驱动未装过; 已装则返回 INVALID_STATE, 忽略) */
    esp_err_t ret = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service: %s", esp_err_to_name(ret));
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

    /* 4. 事件队列 + 中断回调 + 驱动任务 */
    s_queue = xQueueCreate(ENC_QUEUE_LEN, sizeof(encoder_dir_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    gpio_isr_handler_add(ENC_PIN_A, encoder_isr, NULL);
    gpio_isr_handler_add(ENC_PIN_B, encoder_isr, NULL);
    xTaskCreatePinnedToCore(encoder_task, "encoder", ENC_TASK_STACK, NULL,
                            ENC_TASK_PRIO, NULL, ENC_TASK_CORE);

    ESP_LOGI(TAG, "encoder init: A=%d B=%d key_step=%d", ENC_PIN_A, ENC_PIN_B, ENC_KEY_STEP);
    return ESP_OK;
}

void encoder_set_callback(encoder_cb_t cb)
{
    s_cb = cb;
}
