#include "drivers/weather.hpp"

#include <string.h>
#include <stdlib.h>          /* strtol */

#include "esp_log.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "weather";

namespace weather {

/* 最新一份快照 + 脏标记。
 * ★ 写者: websocket_task (回调里)；读者: lvgl 任务 (drain_queues) → 必须上临界区。
 *   用普通互斥量而不是自旋锁: 两边都是任务上下文, 而且持锁时间极短 (只 memcpy)。 */
static SemaphoreHandle_t s_lock = nullptr;
static snapshot_t        s_snap = {};
static volatile bool     s_dirty = false;

/*! 建互斥量。由 UiBridge::init() 调一次 —— 不在 handle() 里懒创建,
 *  因为 handle (websocket_task) 和 take_dirty (lvgl 任务) 可能同时首次进来 → 会建出两个锁。 */
void init(void)
{
    if (s_lock == nullptr) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == nullptr) {
            ESP_LOGE(TAG, "创建互斥量失败");
        }
    }
}

/*! 兜底: 万一有人忘了调 init(), 至少不是空指针崩。 */
static void ensure_lock(void)
{
    if (s_lock == nullptr) {
        s_lock = xSemaphoreCreateMutex();
    }
}

/*! kind → 日志用的中文名 (只用于打印)。 */
static const char *kind_name(int32_t k)
{
    switch (k) {
    case KIND_CLEAR_DAY:   return "白天晴";
    case KIND_CLEAR_NIGHT: return "夜晚晴";
    case KIND_CLOUDY:      return "多云";
    case KIND_OVERCAST:    return "阴天";
    case KIND_RAIN:        return "雨";
    case KIND_SNOW:        return "雪";
    case KIND_THUNDER:     return "雷雨";
    case KIND_WIND:        return "风";
    default:               return "未知";
    }
}

/*! 从 JSON 里取整数字段。★ 网关把 temp 系列发成【字符串】"18", 所以两种形态都要接。 */
static int32_t json_int(cJSON *root, const char *key, int32_t fallback)
{
    cJSON *v = cJSON_GetObjectItem(root, key);
    if (v == nullptr) {
        return fallback;
    }
    if (cJSON_IsNumber(v)) {
        return (int32_t)v->valuedouble;
    }
    if (cJSON_IsString(v) && v->valuestring != nullptr) {
        return (int32_t)strtol(v->valuestring, nullptr, 10);
    }
    return fallback;
}

/*! 把 kind 规整到 1~8。网关约定 999/未知 兜底成 3(多云)。 */
static int32_t normalize_kind(int32_t k)
{
    if (k >= KIND_CLEAR_DAY && k <= KIND_WIND) {
        return k;
    }
    return KIND_CLOUDY;
}

bool handle(const char *payload, int len)
{
    if (payload == nullptr || len <= 0) {
        return false;
    }

    /* ★ ws.cpp 传进来的 payload 不一定以 NUL 结尾 (它是在接收缓冲里就地切分的),
     *   所以这里必须用带长度的解析函数, 不能用 cJSON_Parse。 */
    cJSON *root = cJSON_ParseWithLength(payload, (size_t)len);
    if (root == nullptr) {
        ESP_LOGW(TAG, "JSON 解析失败 (%d 字节)", len);
        return false;
    }

    info_t in = {};
    in.kind     = normalize_kind(json_int(root, "kind", KIND_CLOUDY));
    in.temp     = json_int(root, "temp", 0);
    in.temp_max = json_int(root, "temp_max", 0);
    in.temp_min = json_int(root, "temp_min", 0);

    cJSON *txt = cJSON_GetObjectItem(root, "text");
    if (cJSON_IsString(txt) && txt->valuestring != nullptr) {
        strlcpy(in.text, txt->valuestring, sizeof(in.text));
    }
    cJSON_Delete(root);

    ensure_lock();
    if (s_lock != nullptr) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    /* ★ 值没变就不置脏 —— 免得同一份数据被网关重推时又白刷一次屏 */
    const bool changed = (memcmp(&s_snap.info, &in, sizeof(in)) != 0) || !s_snap.valid;
    if (changed) {
        s_snap.info  = in;
        s_snap.valid = true;
        s_dirty      = true;
    }
    if (s_lock != nullptr) {
        xSemaphoreGive(s_lock);
    }

    if (changed) {
        ESP_LOGI(TAG, "天气: kind=%d(%s) %d℃ (%d~%d) \"%s\"",
                 (int)in.kind, kind_name(in.kind), (int)in.temp,
                 (int)in.temp_min, (int)in.temp_max, in.text);
    }
    return true;
}

void get(snapshot_t *out)
{
    if (out == nullptr) {
        return;
    }
    ensure_lock();
    if (s_lock != nullptr) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    *out = s_snap;
    if (s_lock != nullptr) {
        xSemaphoreGive(s_lock);
    }
}

bool has_data(void)
{
    ensure_lock();
    if (s_lock != nullptr) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    const bool v = s_snap.valid;
    if (s_lock != nullptr) {
        xSemaphoreGive(s_lock);
    }
    return v;
}

bool take_dirty(void)
{
    ensure_lock();
    if (s_lock != nullptr) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    const bool d = s_dirty;
    s_dirty = false;
    if (s_lock != nullptr) {
        xSemaphoreGive(s_lock);
    }
    return d;
}

}  // namespace weather
