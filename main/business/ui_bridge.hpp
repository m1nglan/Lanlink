#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "business/bus_msg.hpp"
#include "drivers/rtasr.hpp"

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

    /* ---------------- 触发接口 (任意上下文可调, 全部非阻塞) ---------------- */

    /*! 请求开始一轮语音会话 (按下录音键 BTN_REC_PIN, 现为 GPIO2) */
    bool voice_start(void);
    /*! 请求结束本轮 (松开录音键) */
    bool voice_stop(void);

    /*! 给 voice_task 用: 拿命令队列句柄, 自己阻塞等 */
    QueueHandle_t voice_queue(void) const { return m_voice_q; }

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

    /*! WS 回调: 服务器确认服务切换完成 → 投 VOICE_SVC_ACKED 唤醒在等的 voice_session。
     *  同 on_asr_result, 跑在 websocket_task 且持锁 → 只做非阻塞投递。 */
    static void on_svc_ok(const char *service, void *ctx);

    /*! WS 回调: 网关报错 ({"type":"error","code":N})。
     *  code 1(讯飞错误) / 3(音频超过55s) 说明这一轮已废 → 投 VOICE_STOP 让采音收尾,
     *  而不是继续白发音频。跑在 websocket_task 且持锁 → 只做非阻塞投递。 */
    static void on_ws_error(const char *payload, int len, void *ctx);

    bool push_voice_cmd(voice_cmd_t cmd, int32_t arg); 

    QueueHandle_t m_voice_q  = nullptr;
    QueueHandle_t m_resp_q   = nullptr;
    QueueHandle_t m_stream_q = nullptr;

    /*! 本轮是否在录音 (由 post_resp_status 的 STARTED/ENDED/ABORTED 维护)。
     *  两个用途: ① on_ws_error 决定要不要投 VOICE_STOP (否则残留的 STOP
     *  会被下一轮开头的 voice_q_drain 当成"用户松手"而取消新会话);
     *  ② 录音期间禁止转屏 (lvgl_port_set_encoder_enabled)。 */
    volatile bool m_asr_active = false;

    RtAsr m_asr;   /*!< 全局常驻实例 (坑#4) */
};
