#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "drivers/button.hpp"
#include "drivers/wifi.hpp"

static const char *TAG = "Main";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=========Main=========");

    /* 连接 WiFi */
    WiFi wifi;
    ESP_ERROR_CHECK(wifi.init());

    /* 等待连接成功(事件回调会把 is_connected 置 true) */
    while (!wifi.is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "WiFi 连接成功");

    /* 使用标准按键驱动(GPIO 配置 + 消抖都封装在 Button 里) */
    Button btn(GPIO_NUM_12);
    ESP_ERROR_CHECK(btn.init());

    bool prev = btn.is_pressed();
    while (1) {
        bool pressed = btn.is_pressed();
        if (pressed != prev) {          /* 状态变化才打印(按下/松开沿) */
            prev = pressed;
            ESP_LOGI(TAG, "按键状态: %s", pressed ? "按下" : "松开");
        }
    }
}
