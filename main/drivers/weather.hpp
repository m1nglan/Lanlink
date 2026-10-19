#pragma once

/* ================================================================
 * weather —— 天气数据（协议解析 + 最新一份的快照）
 *
 * [链] websocket_task → WS::dispatch("weather") → UiBridge::on_weather
 *        → weather::handle(payload, len)        ← 解析 + 存起来 + 置脏标记
 *      lvgl 任务 → drain_queues → weather_view::apply(&w)  ← 上屏
 *
 * ★ 本模块**只认协议、不认 LVGL**（不知道有 ui_weather1 这个控件）。
 * ★ 也不碰队列 —— 数据靠一个"脏标记"交给 UI 任务取：
 *     · 天气是低频数据（5 分钟一次），为它单开一条队列不值得（本项目队列数量已定为 3）
 *     · UI 任务每 50ms 调一次 take_dirty()，代价只是一次 bool 判断
 *   ⇒ 你可能会问"这不是又轮询了吗"：是，但轮的是内存里的一个 flag，不是网络也不是硬件；
 *     真要无轮询，得再开一条队列并把 resp_msg_t 改大，收益不抵风险。
 *
 * ★ 报文格式（见 MD/APIserver.md §6）：
 *     {"type":"weather","weather":true,"text":"多云","temp":"18",
 *      "temp_max":"29","temp_min":"17","kind":3}
 *     kind: 1白天晴 2夜晚晴 3多云 4阴天 5雨 6雪 7雷雨 8风  (999/未知 → 兜底 3 多云)
 *   ⚠️ temp/temp_max/temp_min 在报文里是【字符串】，不是数字。
 * ================================================================ */

#include <stdint.h>

namespace weather {

/*! 天气类型 —— 和网关的 kind 一一对应 (999/未知按网关约定兜底成 CLOUDY) */
enum kind_t : int {
    KIND_CLEAR_DAY   = 1,   /*!< 白天晴 */
    KIND_CLEAR_NIGHT = 2,   /*!< 夜晚晴 */
    KIND_CLOUDY      = 3,   /*!< 多云  (未知时的兜底) */
    KIND_OVERCAST    = 4,   /*!< 阴天 */
    KIND_RAIN        = 5,   /*!< 雨 */
    KIND_SNOW        = 6,   /*!< 雪 */
    KIND_THUNDER     = 7,   /*!< 雷雨 */
    KIND_WIND        = 8,   /*!< 风 */
};

/*! 一份天气快照 */
typedef struct {
    int32_t kind;         /*!< 见 kind_t */
    int32_t temp;         /*!< 当前温度 ℃ (可为负) */
    int32_t temp_max;     /*!< 当日最高 */
    int32_t temp_min;     /*!< 当日最低 */
    char    text[48];     /*!< 中文描述 (UTF-8, 48 字节够放 15 个汉字) */
} info_t;

/*! 初始化（建互斥量）。由 UiBridge::init() 调一次，必须在使用任何其它接口之前。 */
void init(void);

/*! 解析一条 {"type":"weather",...} 的原始报文。
 *  ★ 跑在 websocket_task (持 client->lock) → 里面**不能阻塞**、不能碰 LVGL。
 *  @return true = 解析成功并已更新快照 */
bool handle(const char *payload, int len);

/*! 取最新快照。没有数据时 valid 为 false。线程安全 (内部临界区)。 */
typedef struct {
    bool   valid;      /*!< 是否已经在某条报文里解析到过数据 */
    info_t info;
} snapshot_t;
void get(snapshot_t *out);

/*! 是否已经收到过有效天气数据（线程安全）。
 *  ★ 给启动门用: 主界面在天气到之前显示的是 SquareLine 的占位图/文字
 *    ("18" / "小雨" / cloudy 那张图), 早点等到天气再切屏更自然。 */
bool has_data(void);

/*! 取走"有新数据"的标记 (读到就清掉)。给 UI 任务用。
 *  @return true = 自上次调用以来有更新 */
bool take_dirty(void);

}  // namespace weather
