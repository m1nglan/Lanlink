#include "drivers/button.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "button";

esp_err_t Button::init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << m_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = BTN_PULL_UP_EN,
        .pull_down_en = BTN_PULL_DOWN_EN,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    m_pressed = is_pressed();
    m_last_active = m_pressed;   /* 沿检测的初始状态: 记录当前按下状态 */
    ESP_LOGI(TAG, "button GPIO%d init OK (active level=%d, debounce ~%dms)",
             m_pin, m_active_level, BTN_DEBOUNCE_MS * BTN_DEBOUNCE_N);
    return ESP_OK;
}

int Button::read_level(void)
{
    int last = gpio_get_level(m_pin);
    int n = 0;
    while (n < BTN_DEBOUNCE_N) {
        vTaskDelay(pdMS_TO_TICKS(BTN_DEBOUNCE_MS));
        int cur = gpio_get_level(m_pin);
        if (cur == last) {
            n++;
        } else {
            last = cur;   /* 有抖动,重新计时 */
            n = 0;
        }
    }
    return last;
}

bool Button::is_pressed(void)
{
    m_pressed = (read_level() == m_active_level);
    return m_pressed;
}

bool Button::is_pressed_edge(void)
{
    /* 单次读取 GPIO 电平(不消抖重读,避免长按期间重读抖动误判边沿)。
     * 若与 active_level 相同 = 当前按下。 */
    bool now_active = (gpio_get_level(m_pin) == m_active_level);

    /* 按下沿: 上一次未按下, 本次按下 → 触发一次 */
    bool edge = (now_active && !m_last_active);
    m_last_active = now_active;   /* 记录本次状态,供下次判断 */

    return edge;
}
