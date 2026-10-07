#pragma once

#include <stdint.h>

/* ================================================================
 * 业务总线消息类型 —— 纯类型定义, 不含任何逻辑
 *
 * [链] 生产者 → 【本文件的三条队列】 → 消费者
 *   voice_q   触发类  按键回调/WS回调   → voice_task         (深 4,  满则丢)
 *   resp_q    控制类  上述三者/ws_keeper → lvgl 任务 ui_timer (深 16, 可靠)
 *   stream_q  流式类  websocket_task    → lvgl 任务 ui_timer (深 1,  覆盖)
 *
 * 为什么必须是三条、不能合并 → 详见 MD/HANDOFF.md §6-A
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
    int32_t     arg;     /*!< 标量参数 (含义由每个 cmd 自定义, 当前全传 0)。
                          *  ★ 只有 4 字节: 装不下文本, **也不要塞指针** ——
                          *    那会把"值拷贝"退回成"指针 + 无人保护的内存"(悬空/被改写) */
} voice_cmd_msg_t;       /*!< 8 字节 */


/* ============ 流式通道 stream_q (深度 1, 覆盖式 xQueueOverwrite) ============
 * 语义: 只留最新一句。生产者永不阻塞/永不失败; 消费者"非空才刷"。
 *
 * ★★ 装的**永远是【完整文本】**, 不是增量 (V2 协议, 见 MD/PROTOCOL.md):
 *   ASR 由网关发全量; LLM 由 Llm 驱动侧累积好再投 → 消费者零累积状态。
 *   ⚠️ 累积**必须在发布到总线之前**做完: 本队列覆盖式, 放增量会被覆盖丢掉。 */

typedef enum {
    STREAM_ASR = 0,     /*!< 语音识别: 完整当前句 */
    STREAM_LLM,         /*!< 大模型: 完整回复 (驱动侧已累积好) */
} stream_kind_t;

/*! 长文本容量 —— **只用在 stream_q 上** (深度 1, 覆盖式, 只占一份)。
 *  所以开大很便宜: 512 → 2048 只多花 1.5KB 总内存。 */
#define BUS_TEXT_LEN 2048

/*! 短文本容量 —— resp_q 用 (深 16)。
 *  ★★ 是 64 不是 512, **别照 V1 的老尺寸改回去**: V1 时 final 走 resp_q、要装整句所以给了 512;
 *     V2 之后识别结果全归 stream_q, 那个 512 × 16 = 8384 B 全是空气。详见 HANDOFF §6-A。
 *  ★ 真需要放长文本 → 走 stream_q。resp_q 深 16, 这里每 +1 字节就是 ×16。 */
#define BUS_TEXT_LEN_SHORT 64

typedef struct {
    stream_kind_t kind;
    bool          is_final;   /*!< ★ true = 这句是最终版, UI 可"定格"。把"流式"和"完成"
                                *   合并进一条消息 → resp_q 不再需要 RESP_*_FINAL */
    char          text[BUS_TEXT_LEN];
} stream_msg_t;               /*!< 约 2056 字节 */


/* ================= 控制通道 resp_q (普通队列, 深度 RESP_Q_LEN) =================
 * 语义: 可靠, 逐条处理, 每条都不能丢。
 * ★ 只走【状态】和【短提示】—— 长文本一律走 stream_q (否则深度 16 会被吃掉)。 */

typedef enum {    //< 状态 键
    RESP_WS_STATUS = 0,   /*!< 连接状态 (连上/断开/重连中) → u.sta */
    RESP_ASR_STATUS,      /*!< 会话状态 (开始/结束/中止)   → u.sta */
    RESP_STATUS,          /*!< 通用短提示 (≤21 汉字)     → u.text */
} resp_kind_t;

typedef enum {          //!< 状态值: 投给 resp_q 的"哪一类状态"
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

    /*! 载荷 —— 同一时刻只有一个成员有效 (由 kind 指示)。
     *  ★ 必须是平凡类型、**不得含指针**: 队列靠 memcpy 搬字节, 指针指向的内存不受保护。
     *  ★ 只放【短】内容: resp_q 深 16, 放长文本会吃掉 16 倍内存 (见 BUS_TEXT_LEN_SHORT)。 */
    union {
        char    text[BUS_TEXT_LEN_SHORT];  /*!< 文本类: 带数值的短提示 (≤21 汉字) */
        int32_t i32;                       /*!< 32bit 数值类 (余额/百分比/计数) */
        float   f32;                       /*!< 单精度浮点类 */
        status_kind_t sta;                 /*!< 状态枚举 —— 目前唯一在用的成员 */
    } u;
} resp_msg_t;             /*!< 76 字节 (旧: 512 时代的 524 字节) */


/* ================= 队列深度 ================= */
#define VOICE_Q_LEN  (4)
#define RESP_Q_LEN   (16)
#define STREAM_Q_LEN (1)      /*!< 覆盖式队列必须是 1, 否则 xQueueOverwrite 会断言 */

/* ================================================================
 * 为什么 resp_q 和 stream_q 要分开
 *
 * stream_q 每条都是全量(可能 10Hz), UI 每 ~137ms 才刷一次。若用普通队列:
 *   ① 生产者(WS 回调)正持 client->lock, **绝不能阻塞** → 满就只能丢, 有几率丢 final;
 *   ② 逐条消费全是无用功 —— 第 N 条还没刷就被第 N+1 条取代了。
 * 覆盖式队列一次解决两条, 天然实现"变化检测", 且无竞态(队列内部临界区保证原子拷贝)。
 * ★ 深度 1 → 长文本只占一份 → BUS_TEXT_LEN 才敢开到 2048 (见 HANDOFF §6-A)。
 * ================================================================ */
