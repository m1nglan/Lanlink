#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "drivers/button.hpp"
#include "drivers/wifi.hpp"
#include "drivers/i2s_mic.hpp"
#include "drivers/rtasr.hpp"

static const char *TAG = "Main";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=========Main=========");

    /* 1. 连接 WiFi(本次只测这个) */
    WiFi wifi;
    ESP_ERROR_CHECK(wifi.init());
    while (!wifi.is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "WiFi 连接成功");

    /* 2. 按键 */
    Button btn(GPIO_NUM_12);
    ESP_ERROR_CHECK(btn.init());

    /* 3. 麦克风 */
    I2sMic mic;
    ESP_ERROR_CHECK(mic.init());
    ESP_ERROR_CHECK(mic.start());

    /* 4. RTASR */
    RtAsr asr;
    ESP_ERROR_CHECK(asr.init());
    ESP_LOGI(TAG, "RTASR 连接成功,开始流式语音转写...");

    /* 5. 流式上传 */
    int16_t pcm[I2S_MIC_FRAME_BYTES / 2];
    uint32_t frame = 0;
    while (1) {
        if (mic.read_frame(pcm, sizeof(pcm), 100) == ESP_OK) {
            asr.send_audio((const uint8_t *)pcm, sizeof(pcm), RTASR_SEND_TIMEOUT_MS);
        }
        if ((++frame % 25) == 0) {
            if (btn.is_pressed()) {
                ESP_LOGI(TAG, "按键按下(测试)");
            }
        }
    }
}
