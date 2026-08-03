#include "drivers/rtasr.hpp"

#include <string.h>
#include <time.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_transport.h"
#include "esp_transport_ws.h"
#include "esp_transport_ssl.h"
#include "esp_transport_tcp.h"
#include "esp_crt_bundle.h"
#include "esp_netif_sntp.h"
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
static void build_signa(char *signa, size_t signa_size)
{
    char ts[16];
    snprintf(ts, sizeof(ts), "%lld", (long long)time(NULL));

    char base[64];
    snprintf(base, sizeof(base), "%s%s", XFYUN_APPID, ts);

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

    /* 生成签名 */
    char signa[64];
    build_signa(signa, sizeof(signa));
    if (strncmp(signa, "SIGNA_ERR", 9) == 0) {
        return ESP_FAIL;
    }

    /* 握手参数(严格按文档:appid/ts/signa,可选 lang/vadMdn/pd) */
    char path[256];
    snprintf(path, sizeof(path),
             RTASR_PATH "?appid=%s&ts=%lld&signa=%s&lang=%s&vadMdn=%d%s%s",
             XFYUN_APPID, (long long)time(NULL), signa,
             RTASR_LANG, RTASR_VAD_MDN,
             (RTASR_PD[0] != '\0' ? "&pd=" : ""), RTASR_PD);

    /* 创建传输链:ws 包在 ssl(或 tcp)之上 */
    esp_transport_handle_t parent = NULL;
#if RTASR_USE_TLS
    parent = esp_transport_ssl_init();
    if (parent == NULL) {
        ESP_LOGE(TAG, "ssl transport 创建失败");
        return ESP_ERR_NO_MEM;
    }
    esp_transport_ssl_crt_bundle_attach(parent, esp_crt_bundle_attach);
#else
    parent = esp_transport_tcp_init();
    if (parent == NULL) {
        ESP_LOGE(TAG, "tcp transport 创建失败");
        return ESP_ERR_NO_MEM;
    }
#endif

    esp_transport_handle_t ws = esp_transport_ws_init(parent);
    if (ws == NULL) {
        ESP_LOGE(TAG, "ws transport 创建失败");
        return ESP_ERR_NO_MEM;
    }
    esp_transport_ws_set_path(ws, path);
    m_ws = ws;

    ESP_LOGI(TAG, "连接 %s:%d%s", RTASR_HOST, RTASR_PORT, path);
    if (esp_transport_connect(ws, RTASR_HOST, RTASR_PORT, RTASR_CONNECT_TIMEOUT_MS) != 0) {
        ESP_LOGE(TAG, "连接失败");
        esp_transport_destroy(ws);
        m_ws = NULL;
        return ESP_FAIL;
    }
    int code = esp_transport_ws_get_upgrade_request_status(ws);
    if (code != 101) {
        ESP_LOGE(TAG, "WebSocket 握手失败 HTTP=%d", code);
        esp_transport_destroy(ws);
        m_ws = NULL;
        return ESP_FAIL;
    }
    m_connected = true;
    ESP_LOGI(TAG, "WebSocket 握手成功(101)");

    /* 启动 reader 任务收结果 */
    m_stop = false;
    xTaskCreatePinnedToCore(&RtAsr::reader_task, "rtasr_reader", 6144, this, 5, &m_reader, 1);
    return ESP_OK;
}

esp_err_t RtAsr::send_audio(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (m_ws == NULL || !m_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    int ret = esp_transport_ws_send_raw((esp_transport_handle_t)m_ws,
                                        WS_TRANSPORT_OPCODES_BINARY,
                                        (const char *)data, (int)len, (int)timeout_ms);
    if (ret < 0) {
        ESP_LOGE(TAG, "发送音频失败 ret=%d", ret);
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
    int ret = esp_transport_ws_send_raw((esp_transport_handle_t)m_ws,
                                        WS_TRANSPORT_OPCODES_BINARY,
                                        end, (int)strlen(end), (int)timeout_ms);
    if (ret < 0) {
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
    m_stop = true;
    m_connected = false;
    if (m_reader != NULL) {
        vTaskDelay(pdMS_TO_TICKS(700));   /* 等 reader 在下次读超时退出 */
        if (m_reader != NULL) {
            vTaskDelete(m_reader);
            m_reader = NULL;
        }
    }
    if (m_ws != NULL) {
        esp_transport_destroy((esp_transport_handle_t)m_ws);
        m_ws = NULL;
    }
}

void RtAsr::reader_task(void *arg)
{
    RtAsr *self = static_cast<RtAsr *>(arg);
    self->run_reader();
    self->m_reader = NULL;
    vTaskDelete(NULL);
}

void RtAsr::run_reader(void)
{
    char buf[4096];
    while (!m_stop) {
        int len = esp_transport_read((esp_transport_handle_t)m_ws, buf, sizeof(buf) - 1,
                                     RTASR_READ_TIMEOUT_MS);
        if (len > 0) {
            buf[len] = '\0';
            ws_transport_opcodes_t op = esp_transport_ws_get_read_opcode((esp_transport_handle_t)m_ws);
            if (op == WS_TRANSPORT_OPCODES_TEXT) {
                handle_text(buf, len);
            } else if (op == WS_TRANSPORT_OPCODES_CLOSE) {
                ESP_LOGI(TAG, "服务端关闭连接");
                break;
            }
        } else if (len < 0) {
            ESP_LOGW(TAG, "读取异常(%d),连接断开", len);
            break;
        }
        /* len==0 为读超时,继续循环检查 m_stop */
    }
    m_connected = false;
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
