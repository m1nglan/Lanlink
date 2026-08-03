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
