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
    STREAM_ASR = 0,     /*!< 语音识别: 完整当前句 (我的话, 绿/右) */
    STREAM_LLM,         /*!< 大模型: 完整回复 (驱动侧已累积好, 对方的话, 灰/左) */
    STREAM_ABROAD,      /*!< 网关 {"type":"abroad"} 带出的识别内容 (会话已结束时的"收尾文本")。
                         *  ★★ 为什么必须走 stream_q 而不是 resp_q: 它可能是一整段话
                         *     (用户说了很久, 讯飞那边主动关会话时把已识别的内容一起带出来),
                         *     resp_q 的 u.text 只有 64 字节(≈21 汉字) 会直接截掉大半。
                         *     stream_q 的 BUS_TEXT_LEN = 2048 且深度 1, 装它正合适。 */
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
    RESP_KEY,             /*!< ★ 物理按键原始事件 → u.i32 = (key_id << 1) | pressed */
    RESP_USAGE,           /*!< ★ 服务器监控 → u.usage */
    RESP_BALANCE,         /*!< ★ 余额 → u.f32 = total (元, 2 位小数) */
} resp_kind_t;

/*! 服务器监控快照 (网关 {"type":"usage"}, 切到 usage 服务后每 2 秒自动推送)。
 *
 *  ★★ 为什么能直接塞进 resp_msg_t 的 union: 它只有 42 字节, 而 u.text 本来就占 64
 *     → 加它 **resp_msg_t 不会长大**, resp_q 深 16 也不多花一个字节 ✓
 *
 *  报文 (MD/APIserver.md §5):
 *    {"type":"usage","usage":true,"t":..., "cpu":12,
 *     "ram_used":"1.7G/2G","ram_pct":92,
 *     "disk_used":"33.3G/39.0G","disk_pct":85, "rx":4,"tx":2}
 *  ⚠️ ram_used / disk_used 是【字符串】(带 "G" 和 "/"), 其它都是整数。 */
typedef struct {
    int16_t cpu;           /*!< CPU 占用 %    */
    int16_t ram_pct;       /*!< 内存占用 %    */
    int16_t disk_pct;      /*!< 磁盘占用 %    */
    int16_t rx;            /*!< 下行 KB/s     */
    int16_t tx;            /*!< 上行 KB/s     */
    char    ram_used[16];  /*!< "1.7G/2G"     */
    char    disk_used[16]; /*!< "33.3G/39.0G" */
    char    cpu_str[16];   /*!< ★ "0.3/2 Core" —— 负载/核心数文字, 网关字段名 cpu_str。
                            *    ★ 用字符串而不是数字: 网关想怎么拼就怎么拼 (负载/核心数/
                            *      线程数), 板子这边只负责原样显示 ✓
                            *    16+16=58 字节, 仍不超 u.text 的 64 ✓ */
} usage_t;                 /*!< 58 字节 */

/*! 物理按键编号。
 *
 *  ★★ 这里【只报告"哪个键被按下/松开"】, 绝不判断"按下意味着什么" ——
 *    语义由 LVGL 层按当前 UI 状态决定 (面板开没开、焦点在哪、在哪个屏)。
 *    例: IO42 面板关着时是【录音】(按下开始/松开结束), 面板开着时是【取消】。
 *    两者互斥, 所以不需要长按检测, 也不需要在这里做任何时间判断。
 *  见 UiBridge::drain_queues 里 case RESP_KEY 那一段。 */
typedef enum {
    KEY_A = 0,   /*!< IO2  (原"服务键") —— 打开模型面板 / 确认 */
    KEY_B = 1,   /*!< IO42 (原"录音键") —— 录音 / 取消面板 */
} key_id_t;

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
        usage_t usage;                     /*!< ★ 服务器监控快照 (42 B, 不超 text 的 64 B) */
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
