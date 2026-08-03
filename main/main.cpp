#include <stdio.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
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
    ESP_LOGI(TAG, "空闲堆: %u 字节, 历史最低: %u 字节",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());

    /* 1. 连接 WiFi */
    WiFi wifi;
    ESP_ERROR_CHECK(wifi.init());

    /* 诊断:扫描周围 AP,确认射频正常且能看到 CMCC-360 */
    wifi.scan_and_log();

    while (!wifi.is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "WiFi 连接成功");

    /* 2. 初始化按键(备用/演示) */
    Button btn(GPIO_NUM_12);
    ESP_ERROR_CHECK(btn.init());

    /* 3. 初始化麦克风(16k/16bit/单声道,每帧 1280 字节 = 40ms) */
    I2sMic mic;
    ESP_ERROR_CHECK(mic.init());
    ESP_ERROR_CHECK(mic.start());

    /* 4. 初始化讯飞 RTASR(内部 SNTP 同步时间 + wss 握手) */
    RtAsr asr;
    ESP_ERROR_CHECK(asr.init());
    ESP_LOGI(TAG, "RTASR 连接成功,开始流式语音转写...");

    /* 5. 流式上传:每帧 1280 字节 ≈ 40ms(严格按讯飞文档) */
    int16_t pcm[I2S_MIC_FRAME_BYTES / 2];
    uint32_t frame = 0;
    while (1) {
        if (mic.read_frame(pcm, sizeof(pcm), 100) == ESP_OK) {
            asr.send_audio((const uint8_t *)pcm, sizeof(pcm), RTASR_SEND_TIMEOUT_MS);
        }
        /* 每约 1 秒检查一次按键(演示用,不影响音频节奏) */
        if ((++frame % 25) == 0) {
            if (btn.is_pressed()) {
                ESP_LOGI(TAG, "按键按下(测试)");
            }
        }
    }
}
