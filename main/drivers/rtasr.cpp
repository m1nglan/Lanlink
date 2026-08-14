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

/* ---------------- URL 编码(authorization/date 里的 +/=空格逗号冒号需转义) ---------------- */
static void url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const char *p = src; *p && o + 3 < dst_size; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[o++] = (char)c;
        } else {
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 0xf];
        }
    }
    dst[o] = '\0';
}

/* ---------------- 鉴权: HMAC-SHA256 签名 → authorization(base64) ---------------- */
static esp_err_t build_authorization(char *authorization, size_t auth_size,
                                     char *date_out, size_t date_size)
{
    /* 1. RFC1123 UTC 时间(服务端允许 300s 偏差) */
    time_t now = time(NULL);
    struct tm tm_gmt;
    gmtime_r(&now, &tm_gmt);
    strftime(date_out, date_size, "%a, %d %b %Y %H:%M:%S GMT", &tm_gmt);

    /* 2. 签名原始串: host / date / request-line 三行,换行分隔,冒号后带空格 */
    char sign_origin[160];
    snprintf(sign_origin, sizeof(sign_origin),
             "host: %s\ndate: %s\nGET " RTASR_PATH " HTTP/1.1",
             RTASR_HOST, date_out);

    /* 3. HMAC-SHA256(sign_origin, APISecret) */
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));

    mbedtls_svc_key_id_t key_id = 0;
    psa_status_t st = psa_import_key(&attr,
                                     (const uint8_t *)XFYUN_API_SERCET, strlen(XFYUN_API_SERCET),
                                     &key_id);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key 失败: %d", (int)st);
        return ESP_FAIL;
    }

    unsigned char sig[32];
    size_t sig_len = 0;
    st = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                         (const uint8_t *)sign_origin, strlen(sign_origin),
                         sig, sizeof(sig), &sig_len);
    psa_destroy_key(key_id);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "HMAC-SHA256 失败: %d", (int)st);
        return ESP_FAIL;
    }

    char signature[64];
    base64_encode(sig, sig_len, signature, sizeof(signature));

    /* 4. authorization_origin */
    char auth_origin[320];
    snprintf(auth_origin, sizeof(auth_origin),
             "api_key=\"%s\", algorithm=\"hmac-sha256\", headers=\"host date request-line\", signature=\"%s\"",
             XFYUN_API_KEY, signature);

    /* 5. authorization = base64(auth_origin) */
    base64_encode((const unsigned char *)auth_origin, strlen(auth_origin),
                  authorization, auth_size);

    return ESP_OK;
}

/* ---------------- SNTP 时间同步(鉴权需要当前 UTC 时间) ---------------- */
static esp_err_t sync_time(void)
{
    ESP_LOGI(TAG, "SNTP 同步时间...");
    /* 国内 NTP 服务器(阿里云),比 pool.ntp.org 稳定可达 */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    esp_err_t ret = esp_netif_sntp_init(&cfg);   /* config.start=true → 自动启动 */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SNTP init 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 最多重试 3 次,每次 10s */
    for (int i = 0; i < 3; i++) {
        ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "时间已同步,当前 ts=%lld", (long long)time(NULL));
            return ESP_OK;
        }
        ESP_LOGW(TAG, "同步超时,重试 %d/3...", i + 1);
    }
    ESP_LOGE(TAG, "时间同步失败,鉴权将无效");
    return ESP_ERR_TIMEOUT;
}

/* ======================= 类实现 ======================= */

esp_err_t RtAsr::init(void)
{
    if (m_ws != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(psa_crypto_init(), TAG, "psa_crypto_init failed");
    ESP_RETURN_ON_ERROR(sync_time(), TAG, "time sync failed");

    /* 生成鉴权 authorization 和 date */
    char authorization[384];
    char date[64];
    ESP_RETURN_ON_ERROR(build_authorization(authorization, sizeof(authorization),
                                            date, sizeof(date)),
                        TAG, "build_authorization failed");

    /* authorization/date 含 +/=空格逗号冒号,URL 里需转义 */
    char auth_enc[512];
    char date_enc[128];
    url_encode(authorization, auth_enc, sizeof(auth_enc));
    url_encode(date, date_enc, sizeof(date_enc));

    /* 构建 ws:// URI */
    char uri[768];
    snprintf(uri, sizeof(uri),
             "ws://%s:%d" RTASR_PATH "?authorization=%s&date=%s&host=%s",
             RTASR_HOST, RTASR_PORT, auth_enc, date_enc, RTASR_HOST);

    esp_websocket_client_config_t cfg = {};
    cfg.uri = uri;
    cfg.buffer_size = 8192;                         /*!< 接收缓冲,容纳结果 JSON */
    cfg.network_timeout_ms = RTASR_CONNECT_TIMEOUT_MS;
    cfg.disable_auto_reconnect = true;              /*!< 会话不自动重连,断线交给上层重建 */

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
    m_first_frame_sent = false;
    ESP_LOGI(TAG, "WebSocket 握手成功");
    return ESP_OK;
}

esp_err_t RtAsr::send_audio(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (m_ws == NULL || !m_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 音频 base64 编码(1280 字节 → 约 1708 字符) */
    char audio_b64[1740];
    base64_encode(data, len, audio_b64, sizeof(audio_b64));

    /* 封装 JSON 帧 */
    char frame[2200];
    if (!m_first_frame_sent) {
        /* 首帧: common + business + data(status=0) */
        snprintf(frame, sizeof(frame),
                 "{\"common\":{\"app_id\":\"%s\"},"
                 "\"business\":{\"language\":\"%s\",\"domain\":\"%s\",\"accent\":\"%s\"},"
                 "\"data\":{\"status\":0,\"format\":\"%s\",\"encoding\":\"raw\",\"audio\":\"%s\"}}",
                 XFYUN_APPID, RTASR_LANGUAGE, RTASR_DOMAIN, RTASR_ACCENT,
                 RTASR_AUDIO_FORMAT, audio_b64);
        m_first_frame_sent = true;
    } else {
        /* 中间帧: data(status=1) */
        snprintf(frame, sizeof(frame),
                 "{\"data\":{\"status\":1,\"format\":\"%s\",\"encoding\":\"raw\",\"audio\":\"%s\"}}",
                 RTASR_AUDIO_FORMAT, audio_b64);
    }

    int sent = esp_websocket_client_send_text(m_ws, frame, (int)strlen(frame), timeout_ms);
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
    const char *end = "{\"data\":{\"status\":2}}";
    int sent = esp_websocket_client_send_text(m_ws, end, (int)strlen(end), timeout_ms);
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

    /* 错误: code 非 0(如 401/11200 等) */
    const char *code = strstr(payload, "\"code\":");
    if (code != NULL && code[7] != '0') {
        ESP_LOGE(TAG, "RTASR 错误: %s", payload);
        return;
    }

    /* 提取所有 "w":"文字" 的值拼接 */
    char text[512] = {0};
    size_t o = 0;
    const char *p = payload;
    while (o + 1 < sizeof(text)) {
        p = strstr(p, "\"w\":\"");
        if (p == NULL) {
            break;
        }
        p += 5;  /* 跳过 "w":" */
        while (*p && *p != '"' && o + 1 < sizeof(text)) {
            text[o++] = *p++;
        }
    }
    text[o] = '\0';

    /* 最终结果: result.ls == true */
    bool is_final = (strstr(payload, "\"ls\":true") != NULL);

    if (text[0] != '\0') {
        ESP_LOGI(TAG, "[%s] %s", is_final ? "最终" : "中间", text);
        if (m_cb != NULL) {
            m_cb(text, is_final, m_cb_ctx);
        }
    }
}
