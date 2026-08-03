#include "drivers/i2s_mic.hpp"

#include "esp_log.h"

static const char *TAG = "i2s_mic";

esp_err_t I2sMic::init(void)
{
    if (m_rx != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 1. 创建 I2S RX 通道（主机模式） */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_MIC_PORT_NUM, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = I2S_MIC_DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = I2S_MIC_DMA_FRAME_NUM;

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &m_rx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        m_rx = nullptr;
        return ret;
    }

    /* 2. 标准模式配置：Philips 格式、32bit 槽位、单声道。
     *    底层槽位用 32bit 是为了读取 INMP441 左对齐的 24bit 数据。 */
    i2s_std_gpio_config_t gpio_cfg = {
        .mclk = I2S_GPIO_UNUSED,
        .bclk = I2S_MIC_BCLK_PIN,
        .ws   = I2S_MIC_WS_PIN,
        .dout = I2S_GPIO_UNUSED,
        .din  = I2S_MIC_DIN_PIN,
        .invert_flags = {},
    };

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(I2S_MIC_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = gpio_cfg,
    };
    /* INMP441 的 L/R 引脚接 GND 时为左声道：只启用左槽 */
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ret = i2s_channel_init_std_mode(m_rx, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
        i2s_del_channel(m_rx);
        m_rx = nullptr;
        return ret;
    }

    ESP_LOGI(TAG, "I2S mic init OK: %d Hz / %d bit / mono, frame=%d bytes / %d ms",
             I2S_MIC_SAMPLE_RATE_HZ, I2S_MIC_BITS_PER_SAMPLE,
             I2S_MIC_FRAME_BYTES, I2S_MIC_FRAME_MS);
    return ESP_OK;
}

esp_err_t I2sMic::deinit(void)
{
    if (m_rx == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (m_running) {
        i2s_channel_disable(m_rx);
        m_running = false;
    }
    esp_err_t ret = i2s_del_channel(m_rx);
    m_rx = nullptr;
    return ret;
}

esp_err_t I2sMic::start(void)
{
    if (m_rx == nullptr || m_running) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = i2s_channel_enable(m_rx);
    if (ret == ESP_OK) {
        m_running = true;
        ESP_LOGI(TAG, "I2S mic started");
    } else {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t I2sMic::stop(void)
{
    if (m_rx == nullptr || !m_running) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = i2s_channel_disable(m_rx);
    if (ret == ESP_OK) {
        m_running = false;
        ESP_LOGI(TAG, "I2S mic stopped");
    }
    return ret;
}

esp_err_t I2sMic::read_frame(int16_t *dst, size_t byte_size, uint32_t timeout_ms)
{
    if (m_rx == nullptr || !m_running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (dst == NULL || byte_size == 0 || (byte_size & 1U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (byte_size > I2S_MIC_FRAME_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t samples16 = byte_size / 2;                 /* int16 采样点个数 */
    const size_t raw_bytes = samples16 * sizeof(uint32_t);  /* 底层 32bit 槽位数据量 */

    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(m_rx, m_raw, raw_bytes, &bytes_read, timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }
    if (bytes_read != raw_bytes) {
        ESP_LOGW(TAG, "read timeout: got %u/%u bytes",
                 (unsigned)bytes_read, (unsigned)raw_bytes);
        return ESP_ERR_TIMEOUT;
    }

    /* INMP441 的 24bit 数据左对齐在 32bit 槽位，右移 16 位得到 16bit PCM (s16le) */
    const uint32_t *src = m_raw;
    for (size_t i = 0; i < samples16; i++) {
        dst[i] = (int16_t)(src[i] >> 16);
    }
    return ESP_OK;
}
