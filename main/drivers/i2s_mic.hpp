#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "driver/i2s_std.h"

/* ============================================================================
 * INMP441 I2S 麦克风高层驱动（面向讯飞实时语音转写 S2T 流式上传）
 *
 * 音频输出格式严格遵循「讯飞实时语音转写(标准版)」接口要求：
 *   - 采样率 16kHz
 *   - 位深 16bit（s16le）
 *   - 单声道
 *   - PCM 裸流
 *   - 建议每 40ms 发送 1280 字节
 * ==========================================================================*/

/* ------------------ 音频格式参数（S2T 层依赖，勿随意修改） ------------------ */
#define I2S_MIC_SAMPLE_RATE_HZ      (16000)  /*!< 采样率 16kHz */
#define I2S_MIC_BITS_PER_SAMPLE     (16)     /*!< 位深 16bit */
#define I2S_MIC_NUM_CHANNELS        (1)      /*!< 单声道 */
#define I2S_MIC_FRAME_MS            (40)     /*!< 一帧时长 40ms */
#define I2S_MIC_FRAME_BYTES         (1280)   /*!< 一帧字节数 = 16000*2*1/1000*40 = 1280 */

/* ------------------ 硬件接线参数（按实际连线修改） ------------------ */
#define I2S_MIC_PORT_NUM            (I2S_NUM_0)  /*!< I2S 控制器编号 */
#define I2S_MIC_BCLK_PIN            (GPIO_NUM_11) /*!< INMP441 SCK  -> ESP32 BCLK */
#define I2S_MIC_WS_PIN              (GPIO_NUM_12) /*!< INMP441 WS   -> ESP32 WS(LRCK) */
#define I2S_MIC_DIN_PIN             (GPIO_NUM_13) /*!< INMP441 SD   -> ESP32 DIN */
#define I2S_MIC_DMA_BUF_COUNT       (8)          /*!< DMA 描述符数量 */
#define I2S_MIC_DMA_FRAME_NUM       (256)        /*!< 每个 DMA buffer 的帧数(32bit 槽) */

class I2sMic {
public:
    /*! 初始化 I2S RX 通道（Philips 标准模式，16kHz，32bit 槽位承载 INMP441 24bit 数据） */
    esp_err_t init(void);

    /*! 释放 I2S 通道资源 */
    esp_err_t deinit(void);

    /*! 开始采集（enable 通道，输出 BCLK/WS 时钟） */
    esp_err_t start(void);

    /*! 停止采集（disable 通道） */
    esp_err_t stop(void);

    /*!
     * 读取一帧 s16le 单声道 PCM 数据到 dst。
     * @param dst        输出缓冲（int16 采样点序列）
     * @param byte_size  期望字节数，须为偶数且 ≤ I2S_MIC_FRAME_BYTES
     * @param timeout_ms 阻塞超时（毫秒），S2T 循环中建议 40ms 读一帧 1280 字节
     * @return ESP_OK 成功；ESP_ERR_TIMEOUT 数据不足一帧；ESP_ERR_INVALID_* 参数/状态错误
     */
    esp_err_t read_frame(int16_t *dst, size_t byte_size, uint32_t timeout_ms);

    bool is_running(void) const { return m_running; }

private:
    i2s_chan_handle_t m_rx = nullptr;
    bool m_running = false;
    /* 底层 32bit 槽位数据缓冲改为 static(不在对象/栈上),避免 main 任务栈溢出 */
};

