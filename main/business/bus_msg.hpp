#pragma once

#include <stdint.h>

/* ================================================================
 * 业务总线消息类型 —— **纯类型定义, 不含任何逻辑**
 *
 * 三条通道 (为什么是三条, 见文件末尾的"设计理由"):
 *
 *   cmd_q     触发类   谁想让 voice 干活 → 投这里 (消费者: voice_task)
 *   resp_q    控制类   可靠, 每条都不能丢 (消费者: lvgl 任务)
 *   stream_q  流式类   深度 1 覆盖, 只留最新 (消费者: lvgl 任务)
 *
 * 生产者 / 消费者对照:
 *   cmd_q    生产者 = 按键回调(esp_timer 任务) / 未来 UI 事件
 *            消费者 = voice_task (阻塞 portMAX_DELAY 等它)
 *   resp_q   生产者 = websocket_task (WS 回调里) / voice_task / ws_keeper
 *            消费者 = lvgl 任务的 ui timer
 *   stream_q 生产者 = websocket_task (WS 回调里)
 *            消费者 = lvgl 任务的 ui timer
 * ================================================================ */

/* ================= 命令通道 cmd_q (普通队列, 深度 CMD_Q_LEN) =================
 * 语义: "让谁去干一件事"。投递方永远非阻塞(timeout=0)。 */

typedef enum {
    CMD_NONE = 0,
    CMD_VOICE_START,     /*!< 按下录音键 → 开始一轮语音会话 */
    CMD_VOICE_STOP,      /*!< 松开录音键 → 结束本轮 (voice 在帧边界 peek 到) */
    CMD_SVC_SWITCH,      /*!< IO8 服务键 —— 本阶段只留钩子, 不做业务 */
    CMD_LLM_CHAT_TEXT,   /*!< 预留: 把文本发给 LLM (本阶段不实现) */
} cmd_t;

typedef struct {
    cmd_t   cmd;
    int32_t arg;
} cmd_msg_t;             /*!< 8 字节 */

/* ============ 流式通道 stream_q (深度 1, 覆盖式 xQueueOverwrite) ============
 * 语义: 只留最新一句。生产者**永不阻塞、永不失败**; 消费者"非空才刷"。 */

typedef enum {
    STREAM_ASR_PARTIAL = 0,   /*!< 语音识别增量 (text 服务) */
    STREAM_LLM_PARTIAL,       /*!< LLM 回复增量 (预留) */
} stream_kind_t;

/*! 一句话的容量。生产者侧已由 RtAsr 拼成完整句, 这里按句长 strlcpy(可截断) */
#define BUS_TEXT_LEN 512

typedef struct {
    stream_kind_t kind;
    char          text[BUS_TEXT_LEN];
} stream_msg_t;

/* ================= 控制通道 resp_q (普通队列, 深度 RESP_Q_LEN) =================
 * 语义: 可靠, 逐条处理, 每条都不能丢。 */

typedef enum {
    RESP_ASR_FINAL = 0,   /*!< 语音最终结果 */
    RESP_ASR_STATUS,      /*!< 会话状态 (开始/结束/中止) */
    RESP_WS_STATUS,       /*!< 连接状态 (连上/断开/重连中) */
    RESP_LLM_FINAL,       /*!< 预留 */
    RESP_STATUS,          /*!< 通用提示 */
} resp_kind_t;

typedef struct {
    resp_kind_t kind;
    uint32_t    flags;    /*!< 预留: 附加信息 */
    uint32_t    t_ms;     /*!< 生产者打的时间戳 (esp_timer ms) */
    char        text[BUS_TEXT_LEN];
} resp_msg_t;             /*!< 约 520 字节 */

/* ================= 队列深度 ================= */
#define CMD_Q_LEN    (4)
#define RESP_Q_LEN   (16)
#define STREAM_Q_LEN (1)      /*!< 覆盖式队列必须是 1, 否则 xQueueOverwrite 会断言 */

/* ================================================================
 * 设计理由: 为什么 resp 和 stream 要分两条队列
 *
 * partial 是高频流式(可能 50Hz), 而 UI 每 50ms 才刷一次。
 * 如果都用普通队列, 会同时踩两个坑:
 *   ① 生产者(websocket_task) 正持着 esp_websocket_client 的 client->lock,
 *      **绝对不能阻塞** → 队列满就只能丢 → 有几率把 final 丢掉;
 *   ② 逐条消费全是无用功 —— 第 N 条还没刷就被第 N+1 条取代了。
 *
 * 所以流式走单独的"深度 1 覆盖队列":
 *   生产者 xQueueOverwrite → 永不阻塞、永不失败, 只留最新那句
 *   消费者 xQueueReceive  → 队列非空才刷新; 空则跳过, 不 malloc 不重绘
 *
 * 这天然实现了"变化检测"(不必自己写 flag=1/刷完=0), 且**无竞态**:
 * FreeRTOS 队列内部临界区保证文本随消息原子拷贝、不会撕裂。
 * ================================================================ */
