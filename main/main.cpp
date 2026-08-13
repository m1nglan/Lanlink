#include <stdio.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 二分定位开关:0=禁用,1=启用 */
#define RTASR_ENABLED 0
#define MIC_ENABLED 1
#define BUTTON_ENABLED 0

#include "drivers/button.hpp"
#include "drivers/wifi.hpp"
#include "drivers/i2s_mic.hpp"
#if RTASR_ENABLED
#include "drivers/rtasr.hpp"
#endif

static const char *TAG = "Main";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=========Main=========");
    ESP_LOGI(TAG, "空闲堆: %u 字节, 历史最低: %u 字节",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());

    /* 1. 连接 WiFi(阻塞直到连上或重试耗尽) */
    WiFi wifi;
    ESP_ERROR_CHECK(wifi.init());
    ESP_LOGI(TAG, "WiFi 连接成功");

    /* 2. 按键 */
#if BUTTON_ENABLED
    Button btn(GPIO_NUM_12);
    ESP_ERROR_CHECK(btn.init());
#endif

    /* 3. 麦克风 */
#if MIC_ENABLED
    I2sMic mic;
    ESP_ERROR_CHECK(mic.init());
    ESP_ERROR_CHECK(mic.start());
#endif

    /* 4. RTASR */
#if RTASR_ENABLED
    RtAsr asr;
    ESP_ERROR_CHECK(asr.init());
    ESP_LOGI(TAG, "RTASR 连接成功,开始流式语音转写...");
#endif

    /* 5. 主循环 */
    uint32_t frame = 0;
    while (1) {
#if MIC_ENABLED
        int16_t pcm[I2S_MIC_FRAME_BYTES / 2];
        if (mic.read_frame(pcm, sizeof(pcm), 100) == ESP_OK) {
#if RTASR_ENABLED
            asr.send_audio((const uint8_t *)pcm, sizeof(pcm), RTASR_SEND_TIMEOUT_MS);
#endif
        }
#endif
#if BUTTON_ENABLED
        if ((++frame % 25) == 0) {
            if (btn.is_pressed()) {
                ESP_LOGI(TAG, "按键按下(测试)");
            }
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
