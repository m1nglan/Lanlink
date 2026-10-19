#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "business/bus_msg.hpp"
#include "drivers/rtasr.hpp"
#include "drivers/llm.hpp"

/* ================================================================
 * UiBridge —— 业务总线单例: 三条队列 + asr 回调接线 + UI 消费端
 *
 * [链] 生产者(按键回调/WS回调/voice_task/ws_keeper) →【本类的三条队列】→ 消费者
 *      voice_q → voice_task ｜ resp_q + stream_q → lvgl 任务的 ui_timer
 *
 * 本类**不建连、不建任务**: 连接归 ws_keeper_task, 采音归 voice_task。
 * ⚠️ 所有"投递"接口都非阻塞, 可在**任意上下文**调用 —— 包括 WS 回调,
 *    那时正持 client->lock, 一旦阻塞会拖死整个 WS 收发。
 * ================================================================ */

class UiBridge {
public:
    static UiBridge& get(void);  //< 返回全程序唯一 UiBridge

    /*! 建三队列 + 把 asr 绑到 WS 单例 + 注册结果回调。只做"登记", 与建连无关。 */
    esp_err_t init(void);

    /*! 全局常驻的 RtAsr —— 目前**只有 voice_task** 取它 (ws_keeper 不碰语音)。
     *  ★★ 为什么必须由 UiBridge 持有: init() 把它的**地址**交给了 WS 的回调表
     *     (attach + set_result_callback 都把 this 存了进去) → 生命周期必须 = 系统进程。
     *     放栈上或循环里重建 → 回调踩已释放内存。详见 HANDOFF §6 坑#4。 */
    RtAsr& asr(void) { return m_asr; }

    /*! 当前是否有一轮语音会话在进行 (STARTED 到 ENDED/ABORTED 之间)。
     *
     *  ★ 用途: 天气轮询判断"现在忙不忙" —— 忙就跳过这一轮, 别去抢 svc 状态。
     *  ⚠️ 时序坑: 松手 (ENDED) 那一刻它就变 false 了, 但那时 ask_router 还在等 final
     *     → 所以判"空闲"必须【同时】看 ask_router::is_busy(), 光看这个会误判。
     *  ★★ 还包含"刚按下、voice_task 正在切 svc text"那段窗口 (m_pending_start) ——
     *     那个窗口里 m_asr_active 还是 false, 但这时候切走服务会把整轮语音搞废。 */
    bool is_asr_active(void) const { return m_asr_active || m_pending_start; }

    /* ---------------- 触发接口 (任意上下文可调, 全部非阻塞) ---------------- */

    /*! 请求开始一轮语音会话 (按下录音键 BTN_REC_PIN, 现为 GPIO2) */
    bool voice_start(void);
    /*! 请求结束本轮 (松开录音键) */
    bool voice_stop(void);

    /*! 给 voice_task 用: 拿命令队列句柄, 自己阻塞等 */
    QueueHandle_t voice_queue(void) const { return m_voice_q; }

    /*! ★ 投一条【物理按键原始事件】(按键回调 → resp_q → LVGL 任务)。
     *
     *  ★★ 这里只报告"哪个键按下/松开", 语义判断全在 drain_queues 里 ——
     *    因为只有那里同时看得到"面板开没开"和"是不是在录音"。
     *    例: same KEY_B, 面板关着 = 录音 (按下开始/松开结束); 面板开着 = 取消。
     *    ⚠️ 不要在按键回调里加任何时间/状态判断 —— 那是 LVGL 层的事。
     *  @param key      KEY_A(IO2) / KEY_B(IO42)
     *  @param pressed  true=按下, false=松开
     *  任意上下文可调 (非阻塞)。 */
    void post_key(int key, bool pressed);

    /* ---------------- 生产者接口 (voice_task / ws_keeper 投结果) ---------------- */

    /*! 投一条可靠消息 (满了丢弃并告警)。可在 WS 回调里调用。 */
    void post_resp(resp_kind_t kind, const char *text);

    /*! 投一条**状态**消息: 用统一的 status_kind_t, 不管哪个服务都往这塞。
     *  可在任意上下文调用 (非阻塞)。 */
    void post_resp_status(resp_kind_t kind, status_kind_t status);

    /*! 投一条**已填好**的控制消息 (kind / t_ms / u 都由调用方设好)。
     *  传 const 引用 → 只有 xQueueSend 内部那一次拷贝, 不额外吃栈。加新载荷类型时用这个。 */
    void post_resp_msg(const resp_msg_t &m);

    /*! 投一条流式消息 (覆盖式, 永不失败)。可在 WS 回调里调用。
     *  ★ V2: text **永远是【完整文本】**, 消费者零累积状态; is_final = 可以"定格"。 */
    void post_stream(stream_kind_t kind, const char *text, bool is_final = false);

    /* ---------------- 消费者 (LVGL 侧) ---------------- */

    /*! 注册 UI 消费定时器 (lv_timer, 周期 50ms)。
     *  ★ 必须在持有 lvgl_port_lock() 的情况下调用。 */
    esp_err_t start_ui_timer(void);

    /*! 把两通道队列取出来消费 (上屏 + 状态分派)。
     *  ★ 在 LVGL 上下文内被调用, 此时已持 LVGL 锁 →
     *    内部**绝对不能**再调 lvgl_port_lock() (非递归锁会自死锁)。 */
    void drain_queues(void);

private:
    UiBridge() = default;
    ~UiBridge() = default;
    UiBridge(const UiBridge&) = delete;
    UiBridge& operator=(const UiBridge&) = delete;

    /*! WS 回调 (跑在 websocket_task, 且持着 client->lock) → 投队列 */
    static void on_asr_result(const char *text, bool is_final, void *ctx);

    /*! WS 回调: LLM 流式文本。Llm 驱动已把增量累积成【完整回复】才回调这里,
     *  所以本函数同样只做非阻塞投递 (STREAM_LLM)。 */
    static void on_llm_stream(const char *text, bool is_final, void *ctx);

    /*! WS 回调: 服务器确认服务切换完成 → 投 VOICE_SVC_ACKED 唤醒在等的 voice_session。
     *  同 on_asr_result, 跑在 websocket_task 且持锁 → 只做非阻塞投递。 */
    static void on_svc_ok(const char *service, void *ctx);

    /*! WS 回调: 网关报错 ({"type":"error","code":N})。
     *  code 1(讯飞错误) / 3(音频超过55s) 说明这一轮已废 → 投 VOICE_STOP 让采音收尾,
     *  而不是继续白发音频。跑在 websocket_task 且持锁 → 只做非阻塞投递。 */
    static void on_ws_error(const char *payload, int len, void *ctx);

    /*! WS 回调: 网关的 {"type":"abroad","text":...} —— 讯飞那边的会话已经结束。
     *  ★ text 为 null  → 本轮到此为止, 投 VOICE_STOP 让采音走【正常收尾】
     *                    (不是 abort: asr.end 照发, final 还能回来)
     *  ★ text 为字符串 → 只是提示信息, 投 RESP_STATUS (将来做弹窗用)
     *  跑在 websocket_task 且持锁 → 只做非阻塞投递。 */
    static void on_abroad(const char *payload, int len, void *ctx);

    /*! WS 回调: 网关的 {"type":"weather",...}（svc weather 后自动推一次）。
     *  跑在 websocket_task 且持锁 → 只做解析+存快照，不碰 LVGL。
     *  上屏由 drain_queues 看到 weather::take_dirty() 后调 weather_view::apply()。 */
    static void on_weather(const char *payload, int len, void *ctx);

    /*! WS 回调: 网关的 {"type":"usage",...}（切到 usage 服务后每 2 秒自动推）。
     *  跑在 websocket_task 且持锁 → 只解析 + 投 resp_q，不碰 LVGL。
     *  ★ 载荷塞在 resp_msg_t 的 u.usage 里（42 字节，不超原来的 64 字节 union）✓ */
    static void on_usage(const char *payload, int len, void *ctx);

    /*! WS 回调: 网关的 {"type":"balance",...}（切到 balance 服务后推一次, 之后靠 get）。
     *  跑在 websocket_task 且持锁 → 只解析 + 投 resp_q，不碰 LVGL。
     *  ★ 载荷用 resp_msg_t 的 u.f32（余额本身就是单精度够用）✓ */
    static void on_balance(const char *payload, int len, void *ctx);

    bool push_voice_cmd(voice_cmd_t cmd, int32_t arg); 

    /* ---------------- ask_router 的三个注入回调 ----------------
     * ★ 必须是 static 成员 (ask_router 用 C 风格函数指针, 捕获 lambda 不行);
     *   内部靠 UiBridge::get() 拿到单例, 所以不需要 ctx。
     * ★ "什么时候问"这条策略全在 ask_router 里, 这三个只负责"怎么问"。 */
    static esp_err_t ar_switch_service(const char *service);   /*!< 让 Llm 发 {"type":"svc",...} */
    static esp_err_t ar_send_chat(const char *content);        /*!< 让 Llm 发 {"type":"chat",...} */
    static void      ar_show_waiting(const char *text);        /*!< 投一条占位文字到 stream_q */

    QueueHandle_t m_voice_q  = nullptr;
    QueueHandle_t m_resp_q   = nullptr;
    QueueHandle_t m_stream_q = nullptr;

    /*! 本轮是否在录音 (由 post_resp_status 的 STARTED/ENDED/ABORTED 维护)。
     *  两个用途: ① on_ws_error 决定要不要投 VOICE_STOP (否则残留的 STOP
     *  会被下一轮开头的 voice_q_drain 当成"用户松手"而取消新会话);
     *  ② 录音期间禁止转屏 (lvgl_port_set_encoder_enabled)。 */
    volatile bool m_asr_active = false;

    /*! "已请求开始、但 voice_task 还没切完服务" 的窗口标志。
     *
     *  ★★ 为什么需要: voice_task 会话开头要 set_service("text") + 等 svc_ok
     *     (VOICE_SVC_WAIT_MS = 300ms), 而 m_asr_active 要等 post_resp_status(STARTED)
     *     才置 true —— 那 300ms 里 is_asr_active() 会返回 false。
     *     如果这时候天气轮询切走 svc weather, 网关的服务就变成 weather 了,
     *     voice_task 却以为 text 切好了 → 发 start 到错的服务 → 整轮语音静默失效。
     *  ⇒ voice_start() 置 true, post_resp_status(STARTED) 清掉 (转由 m_asr_active 接管)。
     */
    volatile bool m_pending_start = false;

    RtAsr m_asr;   /*!< 全局常驻实例 (坑#4) */
    Llm   m_llm;   /*!< 同上: 地址交给了 WS 回调表, 生命周期必须是进程级 */
};
