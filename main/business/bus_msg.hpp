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
 * 语义: 只留最新一句。生产者**永不阻塞、永不失败**; 消费者"非空才刷"。
 *
 * ★★ V2 协议 (见 PROTOCOL.md): 这里装的**永远是【完整文本】**, 不是增量。
 *   - ASR: 网关每次发完整当前句 ({"type":"asr"}) → 板子直接替换
 *   - LLM: 网关发增量, 但**累积在 Llm 驱动里做完** → 送进队列的已是完整回复
 *   所以消费者【零累积状态】: 收到什么就 lv_label_set_text 什么。
 *
 *   ⚠️ 这就是"LLM 的累积必须在驱动侧做"的原因: 本队列深度 1 覆盖式,
 *      若把增量放进来等 UI 侧累积 → 消费者慢一点中间的增量就被覆盖丢掉了。 */

typedef enum {
    STREAM_ASR = 0,     /*!< 语音识别: 完整当前句 */
    STREAM_LLM,         /*!< 大模型: 完整回复 (驱动侧已累积好) */
} stream_kind_t;

/*! 长文本容量 —— **只用在 stream_q 上** (深度 1, 覆盖式, 只占一份)。
 *  所以开大很便宜: 512 → 2048 只多花 1.5KB 总内存。 */
#define BUS_TEXT_LEN 2048

/*! 短文本容量 —— resp_q 用 (深 16)。
 *
 *  ★★ 这个值是 64, 不是 512 —— 别照 V1 的老尺寸改回去。
 *     历史: V1 时代 RESP_ASR_FINAL 走 resp_q, 完整句子塞在 u.text 里 → 需要 512。
 *           V2 之后识别结果/LLM 回复全归 stream_q (深 1, BUS_TEXT_LEN=2048),
 *           resp_q 退化成**纯状态通道** → u.text 只剩"带个数值的短提示"用途。
 *           而 512 × RESP_Q_LEN(16) = 8384 字节, 占了队列总内存的 80%,
 *           其中 99% 是空气。
 *
 *  ★ 64 字节 ≈ 21 个汉字, 够放 "服务切换超时" / "音频超过 55 秒上限" 这类提示。
 *    改小之后 resp_msg_t 从 524 → 76 字节, resp_q 从 8384 → 1216 字节。
 *
 *  ★ 真需要放长文本时, 走 stream_q, **不要把这个值调大** ——
 *    resp_q 深 16, 这里每 +1 字节就是 ×16。 */
#define BUS_TEXT_LEN_SHORT 64

typedef struct {
    stream_kind_t kind;
    bool          is_final;   /*!< ★ true = 这句是最终版, UI 可以"定格"。
                                *   把"流式"和"完成"合并到同一条消息里,
                                *   于是 resp_q 不再需要 RESP_ASR_FINAL/RESP_LLM_FINAL。 */
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

typedef enum {          // <向resq_q投的状态信息
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
     *    队列靠 memcpy 搬字节, 它只保护这块内存本身,
     *    指针指向的内存不受队列保护 (会悬空/被改写)。
     *  ★ 这里只放【短】内容 (≤BUS_TEXT_LEN_SHORT=64): resp_q 深 16, 放长文本会吃掉 16 倍内存。
     *    长文本(识别结果/LLM 回复)一律走 stream_q (深 1, BUS_TEXT_LEN=2048), 见 PROTOCOL.md。 */
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
 * 设计理由: 为什么 resp 和 stream 要分两条队列 (V2 协议)
 *
 * stream_q 装【完整文本】(可能 10Hz), 而 UI 每 ~137ms 才刷一次。
 * 如果都用普通队列, 会同时踩两个坑:
 *   ① 生产者(websocket_task) 正持着 esp_websocket_client 的 client->lock,
 *      **绝对不能阻塞** → 队列满就只能丢 → final 有几率被丢;
 *   ② 逐条消费全是无用功 —— 第 N 条还没刷就被第 N+1 条取代了
 *      (V2 里每条都是全量, 前面几条本来就该被取代)。
 *
 * 所以流式走单独的"深度 1 覆盖队列":
 *   生产者 xQueueOverwrite → 永不阻塞、永不失败, 只留最新那句
 *   消费者 xQueueReceive  → 队列非空才刷新; 空则跳过, 不 malloc 不重绘
 *
 * 这天然实现了"变化检测"(不必自己写 flag=1/刷完=0), 且**无竞态**:
 * FreeRTOS 队列内部临界区保证文本随消息原子拷贝、不会撕裂。
 *
 * ★ 深度 1 的另一个好处: 长文本只占【一份】内存 →
 *   BUS_TEXT_LEN 才敢开到 2048 (512 时只多花 1.5KB 总内存)。
 *   如果长文本走深度 16 的 resp_q, 那就是 16 × 2048 = 32KB, 不可接受。
 * ================================================================ */
