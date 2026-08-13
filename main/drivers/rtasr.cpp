#include "drivers/rtasr.hpp"

#include <string.h>
#include <time.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_websocket_client.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "psa/crypto.h"

#include "apikey.h"

static const char *TAG = "rtasr";

/* ---------------- base64 编码(自实现,避免额外组件依赖) ---------------- */
static const char B64_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const unsigned char *src, size_t slen, char *dst, size_t dst_size)
{
    size_t i = 0, o = 0;
    while (i + 3 <= slen && o + 4 < dst_size) {
        uint32_t n = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8) | src[i + 2];
        dst[o++] = B64_TABLE[(n >> 18) & 0x3f];
        dst[o++] = B64_TABLE[(n >> 12) & 0x3f];
        dst[o++] = B64_TABLE[(n >> 6) & 0x3f];
        dst[o++] = B64_TABLE[n & 0x3f];
        i += 3;
    }
    size_t rem = slen - i;
    if (rem == 1) {
        uint32_t n = (uint32_t)src[i] << 16;
        dst[o++] = B64_TABLE[(n >> 18) & 0x3f];
        dst[o++] = B64_TABLE[(n >> 12) & 0x3f];
        dst[o++] = '=';
        dst[o++] = '=';
    } else if (rem == 2) {
        uint32_t n = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8);
        dst[o++] = B64_TABLE[(n >> 18) & 0x3f];
        dst[o++] = B64_TABLE[(n >> 12) & 0x3f];
        dst[o++] = B64_TABLE[(n >> 6) & 0x3f];
        dst[o++] = '=';
    }
    dst[o] = '\0';
}

/* ---------------- 签名:signa = base64(HmacSHA1(MD5(appid+ts), api_key)) ---------------- */
static void build_signa(time_t ts, char *signa, size_t signa_size)
{
    char ts_str[16];
    snprintf(ts_str, sizeof(ts_str), "%lld", (long long)ts);

    char base[64];
    snprintf(base, sizeof(base), "%s%s", XFYUN_APPID, ts_str);

    unsigned char md5raw[16];
    size_t md5len = 0;
    psa_status_t st = psa_hash_compute(PSA_ALG_MD5,
                                       (const uint8_t *)base, strlen(base),
                                       md5raw, sizeof(md5raw), &md5len);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "MD5 计算失败: %d", (int)st);
        snprintf(signa, signa_size, "SIGNA_ERR");
        return;
    }

    char md5hex[33];
    for (int i = 0; i < 16; i++) {
        snprintf(md5hex + i * 2, 3, "%02x", md5raw[i]);
    }

    unsigned char hmac[32];
    size_t hmaclen = 0;

    /* mbedtls 的 psa_mac_compute 需要先导入密钥得到 key ID */
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_1));

    mbedtls_svc_key_id_t key_id = 0;
    st = psa_import_key(&attr,
                        (const uint8_t *)XFYUN_API_KEY, strlen(XFYUN_API_KEY),
                        &key_id);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key 失败: %d", (int)st);
        snprintf(signa, signa_size, "SIGNA_ERR");
        return;
    }

    st = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_1),
                         (const uint8_t *)md5hex, 32,
                         hmac, sizeof(hmac), &hmaclen);
    psa_destroy_key(key_id);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "HMAC-SHA1 计算失败: %d", (int)st);
        snprintf(signa, signa_size, "SIGNA_ERR");
        return;
    }

    base64_encode(hmac, hmaclen, signa, signa_size);
}

/* ---------------- SNTP 时间同步(签名需要当前 Unix 时间戳) ---------------- */
static esp_err_t sync_time(void)
{
    ESP_LOGI(TAG, "SNTP 同步时间...");
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t ret = esp_netif_sntp_init(&cfg);   /* config.start=true → 自动启动 */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SNTP init 失败: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "时间同步失败/超时,签名将无效");
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "时间已同步,当前 ts=%lld", (long long)time(NULL));
    return ESP_OK;
}

/* ======================= 类实现 ======================= */

esp_err_t RtAsr::init(void)
{
    if (m_ws != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(psa_crypto_init(), TAG, "psa_crypto_init failed");
    ESP_RETURN_ON_ERROR(sync_time(), TAG, "time sync failed");

    /* 生成签名(统一时间戳:signa 与 URL 必须用同一个 ts,否则 10110 illegal signa) */
    time_t ts = time(NULL);
    char signa[64];
    build_signa(ts, signa, sizeof(signa));
    if (strncmp(signa, "SIGNA_ERR", 9) == 0) {
        return ESP_FAIL;
    }

    /* 构建 ws:// URI(签名等参数放 query,esp_websocket_client 会自动解析) */
    char uri[320];
    snprintf(uri, sizeof(uri),
             "ws://%s:%d" RTASR_PATH "?appid=%s&ts=%lld&signa=%s&lang=%s&vadMdn=%d%s%s",
             RTASR_HOST, RTASR_PORT,
             XFYUN_APPID, (long long)ts, signa,
             RTASR_LANG, RTASR_VAD_MDN,
             (RTASR_PD[0] != '\0' ? "&pd=" : ""), RTASR_PD);

    esp_websocket_client_config_t cfg = {};
    cfg.uri = uri;
    cfg.buffer_size = 4096;                         /*!< 接收缓冲,容纳 RTASR 结果 */
    cfg.network_timeout_ms = RTASR_CONNECT_TIMEOUT_MS;
    cfg.disable_auto_reconnect = true;              /*!< RTASR 会话不自动重连,断线交给上层重建 */

    m_ws = esp_websocket_client_init(&cfg);
    if (m_ws == NULL) {
        ESP_LOGE(TAG, "esp_websocket_client_init 失败");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_websocket_register_events(m_ws, WEBSOCKET_EVENT_ANY,
                                                  &RtAsr::ws_event_handler, this);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册事件失败: %s", esp_err_to_name(ret));
        esp_websocket_client_destroy(m_ws);
        m_ws = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "连接 %s ...", uri);
    ret = esp_websocket_client_start(m_ws);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动失败: %s", esp_err_to_name(ret));
        esp_websocket_client_destroy(m_ws);
        m_ws = NULL;
        return ret;
    }

    /* 等待 WebSocket 握手完成(连接成功) */
    uint32_t deadline = (uint32_t)(esp_timer_get_time() / 1000) + RTASR_CONNECT_TIMEOUT_MS;
    while (!esp_websocket_client_is_connected(m_ws)) {
        if ((uint32_t)(esp_timer_get_time() / 1000) > deadline) {
            ESP_LOGE(TAG, "连接超时");
            esp_websocket_client_destroy(m_ws);
            m_ws = NULL;
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    m_connected = true;
    ESP_LOGI(TAG, "WebSocket 握手成功(101)");
    return ESP_OK;
}

esp_err_t RtAsr::send_audio(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (m_ws == NULL || !m_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    int sent = esp_websocket_client_send_bin(m_ws, (const char *)data, (int)len, timeout_ms);
    if (sent < 0) {
        ESP_LOGE(TAG, "发送音频失败 ret=%d", sent);
        m_connected = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t RtAsr::finish(uint32_t timeout_ms)
{
    if (m_ws == NULL || !m_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *end = "{\"end\": true}";
    int sent = esp_websocket_client_send_bin(m_ws, end, (int)strlen(end), timeout_ms);
    if (sent < 0) {
        m_connected = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void RtAsr::set_result_callback(rtasr_result_cb_t cb, void *user_ctx)
{
    m_cb = cb;
    m_cb_ctx = user_ctx;
}

void RtAsr::deinit(void)
{
    m_connected = false;
    if (m_ws != NULL) {
        esp_websocket_client_stop(m_ws);
        esp_websocket_client_destroy(m_ws);
        m_ws = NULL;
    }
}

void RtAsr::ws_event_handler(void *handler_args, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    (void)base;
    RtAsr *self = static_cast<RtAsr *>(handler_args);
    if (self == nullptr) {
        return;
    }

    esp_websocket_event_data_t *data = static_cast<esp_websocket_event_data_t *>(event_data);
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
        self->m_connected = true;
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        self->m_connected = false;
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WEBSOCKET_EVENT_ERROR");
        self->m_connected = false;
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data != nullptr && data->op_code == WS_TRANSPORT_OPCODES_TEXT) {
            self->handle_text(data->data_ptr, data->data_len);
        }
        break;
    default:
        break;
    }
}

void RtAsr::handle_text(const char *payload, int len)
{
    (void)len;
    if (strstr(payload, "\"action\":\"started\"")) {
        ESP_LOGI(TAG, "RTASR 握手成功(started)");
        return;
    }
    if (strstr(payload, "\"action\":\"error\"")) {
        ESP_LOGE(TAG, "RTASR 错误: %s", payload);
        return;
    }
    if (strstr(payload, "\"action\":\"result\"")) {
        char text[512] = {0};
        bool is_final = true;
        /* 中间结果 type=1(兼容转义/非转义两种形式) */
        if (strstr(payload, "\\\"type\\\":\\\"1\\\"") != NULL ||
            strstr(payload, "\"type\":\"1\"") != NULL) {
            is_final = false;
        }

        /* 提取所有 w 字段的值拼接成识别文本 */
        const char *p = payload;
        size_t o = 0;
        while (*p && o + 1 < sizeof(text)) {
            const char *a = strstr(p, "\\\"w\\\":\\\"");
            const char *b = strstr(p, "\"w\":\"");
            const char *hit = NULL;
            if (a != NULL && (b == NULL || a < b)) hit = a;
            else if (b != NULL) hit = b;
            if (hit == NULL) break;

            const bool escaped = (hit == a);
            const char *val = escaped ? hit + 7 : hit + 5;
            const char *q = val;
            if (escaped) {
                /* 转义形式:值以 \" 结束 */
                while (*q && o + 1 < sizeof(text)) {
                    if (*q == '\\' && q[1] == '"') break;
                    if (*q == '\\' && q[1] == '\\') { text[o++] = '\\'; q += 2; continue; }
                    text[o++] = *q++;
                }
                p = q + 2;
            } else {
                /* 普通形式:值以 " 结束 */
                while (*q && *q != '"' && o + 1 < sizeof(text)) {
                    text[o++] = *q++;
                }
                p = q + 1;
            }
        }
        text[o] = '\0';

        if (text[0] != '\0') {
            ESP_LOGI(TAG, "[%s] %s", is_final ? "最终" : "中间", text);
            if (m_cb != NULL) {
                m_cb(text, is_final, m_cb_ctx);
            }
        }
    }
}
