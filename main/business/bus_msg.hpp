#pragma once

#include <stdint.h>

/* ================================================================
 * 业务总线消息类型 —— **纯类型定义, 不含任何逻辑**
 *
 * 三条通道 (为什么是三条, 见文件末尾的"设计理由"):
 *
 *   voice_q   触发类   谁想让 voice 干活 → 投这里 (消费者: voice_task)
 *   resp_q    控制类   可靠, 每条都不能丢 (消费者: lvgl 任务)
 *   stream_q  流式类   深度 1 覆盖, 只留最新 (消费者: lvgl 任务)
 *
 * 生产者 / 消费者对照:
 *   voice_q  生产者 = 按键回调(esp_timer 任务) / 未来 UI 事件
 *            消费者 = voice_task (阻塞 portMAX_DELAY 等它)
 *   resp_q   生产者 = websocket_task (WS 回调里) / voice_task / ws_keeper
 *            消费者 = lvgl 任务的 ui timer
 *   stream_q 生产者 = websocket_task (WS 回调里)
 *            消费者 = lvgl 任务的 ui timer
 * ================================================================ */


/* ========== 命令通道 voice_q (普通队列, 深度 VOICE_Q_LEN) ==========
 * 语义: "让 voice 去干一件事"。投递方永远非阻塞(timeout=0)。
 * ★ 消费者只有 voice_task 一个任务 —— 所以它就叫 voice_q */

typedef enum {
    VOICE_NONE = 0,
    VOICE_START,             /*!< 按下录音键 → 开始一轮语音会话 */
    VOICE_STOP,              /*!< 松开录音键 → 结束本轮 (voice 在帧边界 peek 到) */
    VOICE_LLM_CHAT_TEXT,     /*!< 预留: 把文本发给 LLM (本阶段不实现) */
    VOICE_SVC_ACKED      /*!< 服务器已确认服务切换完成 (ws.cpp 收到 svc_ok 时投递) */
} voice_cmd_t;

typedef struct {
    voice_cmd_t cmd;
    int32_t     arg;     /*!< 命令的**标量参数** —— 含义由每个 cmd 自己定义, 当前全传 0。
                          *  ★ 只有 4 字节: 装不下文本;
                          *  ★ 不要拿它塞指针 —— 那等于把"值拷贝"退回成
                          *    "指针 + 一块无人保护的内存"(悬空/被改写)。 */
} voice_cmd_msg_t;       /*!< 8 字节 */


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

typedef enum {    //< 状态 键
    RESP_ASR_FINAL = 0,   /*!< 语音最终结果 */
    RESP_ASR_STATUS,      /*!< 会话状态 (开始/结束/中止) */
    RESP_WS_STATUS,       /*!< 连接状态 (连上/断开/重连中) */
    RESP_LLM_FINAL,       /*!< 预留 */
    RESP_STATUS,          /*!< 通用提示 */
} resp_kind_t;

typedef enum {          // <向resq_q投的状态信息 给voice用
    CONNECTED = 0,
    DISCONNECTED,
    RECONNECTING,
    STARTED,
    ENDED,
    ABORTED,
} status_kind_t;

typedef struct {
    resp_kind_t kind;     /*!< ★ 标签: 决定 u 里哪一项有效 */
    uint32_t    flags;    /*!< 预留: 附加信息 */
    uint32_t    t_ms;     /*!< 生产者打的时间戳 (esp_timer ms) */

    /*! 载荷 —— 同一时刻**只有一个成员有效**, 由 kind 指示。
     *  ★ 成员必须是"平凡类型"(纯 C 值), 且**不得含指针**:
     *    队列靠 memcpy 搬字节, 它只保护这 512 字节本身,
     *    指针指向的内存不受队列保护 (会悬空/被改写)。
     *  ★ 新增类型只要 ≤ BUS_TEXT_LEN, 就往这里加一项 ——
     *    队列深度、UiBridge、内存占用全都不用动。 */
    union {
        char    text[BUS_TEXT_LEN];   /*!< 文本类 (512 字节) */
        int32_t i32;                  /*!< 32bit 数值类 (余额/百分比/计数) */
        float   f32;                  /*!< 单精度浮点类 */
        status_kind_t sta;
    } u;
} resp_msg_t;             /*!< 524 字节 —— union 取最大成员(512), 与原 text[512] 等大 */


/* ================= 队列深度 ================= */
#define VOICE_Q_LEN  (4)
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
