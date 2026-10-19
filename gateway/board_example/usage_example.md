/*
 * 板端对接示例 — 按键触发一轮语音识别
 *
 * 思路: 开机连一次网关(长连接) → 按键按下 start+录音 → 松开 end → 收 final 显示
 *
 * 依赖: esp_websocket_client 组件 (idf_component.yml 已有)
 * CMakeLists.txt 的 REQUIRES 加: esp_websocket_client
 */

#include "gateway_client.hpp"

static GatewayClient s_gw;

/* 录音任务: 按键按下时启动 */
static void record_task(void *arg)
{
    // 假设 I2sMic 已有 read_frame (16kHz/16bit/mono)
    extern void mic_read(int16_t *buf, size_t samples, uint32_t timeout_ms);

    int16_t pcm[640];  // 1280B = 40ms

    s_gw.start_round();
    while (button_is_pressed()) {          // 按键按住期间持续录音
        mic_read(pcm, 640, 100);
        s_gw.send_audio((const uint8_t *)pcm, sizeof(pcm));
        vTaskDelay(pdMS_TO_TICKS(10));     // 保证 ~40ms 间隔
    }
    s_gw.end_round();                      // 松开发结束触发器
    vTaskDelete(NULL);
}

extern "C" void app_main(void)
{
    /* 1. WiFi 连接(现有代码) */
    // wifi.init();

    /* 2. 连网关(长连接, 内部自动重连) */
    s_gw.on_partial = [](const std::string &t) {
        ESP_LOGI("APP", "partial: %s", t.c_str());
    };
    s_gw.on_final = [](const std::string &t) {
        ESP_LOGI("APP", "final: %s", t.c_str());
        // 这里把文本显示到屏幕/播TTS等
    };
    ESP_ERROR_CHECK(s_gw.connect(
        "ws://39.104.84.177:18888/iat?token=lanlink2026"));

    /* 3. 按键按下 → 开一轮识别 */
    while (1) {
        if (button_is_pressed()) {
            xTaskCreate(record_task, "record", 4096, NULL, 5, NULL);
            while (button_is_pressed()) vTaskDelay(pdMS_TO_TICKS(50));
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
