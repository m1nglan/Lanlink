#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "business/bus_msg.hpp"
#include "drivers/rtasr.hpp"

/* ================================================================
 * UiBridge —— 业务总线单例: 三条队列 + asr 回调接线 + UI 消费端
 *
 * 分工:
 *   - 队列的**生产者**: 按键回调(esp_timer 任务) / WS 回调(websocket_task)
 *                     / voice_task / ws_keeper
 *   - 队列的**消费者**: voice_task(取 voice_q) / lvgl 任务(取 resp_q, stream_q)
 *
 * 本类**不建连、不建任务**:
 *   连接生命周期归 ws_keeper_task, 采音归 voice_task。
 *
 * ⚠️ 线程安全约定: 本类所有"投递"接口都是非阻塞的, 可在**任意上下文**调用,
 *    包括 WS 回调 —— 那时正持着 esp_websocket_client 的 client->lock,
 *    一旦阻塞会拖死整个 WS 收发。
 * ================================================================ */

class UiBridge {
public:
    static UiBridge& get(void);  //< 返回全程序唯一Uibridge

    /*! 建三队列 + 把 asr 绑到 WS 单例 + 注册结果回调。
     *  只做"登记", 与是否已建连无关 → 全程调用一次即可。 */
    esp_err_t init(void);

    /*! 全局常驻的 RtAsr (坑#4: 回调 ctx 生命周期 = 系统进程)。
     *  voice_task / ws_keeper 都从这里拿同一个实例。 */
    RtAsr& asr(void) { return m_asr; }

    /* ---------------- 触发接口 (任意上下文可调, 全部非阻塞) ---------------- */

    /*! 请求开始一轮语音会话 (按下 IO10) */
    bool voice_start(void);
    /*! 请求结束本轮 (松开 IO10) */
    bool voice_stop(void);

    /*! 给 voice_task 用: 拿命令队列句柄, 自己阻塞等 */
    QueueHandle_t voice_queue(void) const { return m_voice_q; }

    /* ---------------- 生产者接口 (voice_task / ws_keeper 投结果) ---------------- */

    /*! 投一条可靠消息 (满了丢弃并告警)。可在 WS 回调里调用。 */
    void post_resp(resp_kind_t kind, const char *text);

    /*! 投一条**状态**消息: kind 标注是哪类状态 (决定种类), status 用统一的
     *  status_kind_t —— 不管哪个服务, 要报给 LVGL 的状态都往这里塞。
     *  可在任意上下文调用 (非阻塞)。 */
    void post_resp_status(resp_kind_t kind, status_kind_t status);

    /*! 投一条**已填好**的控制消息: kind / t_ms / u 都由调用方设好。
     *  传 const 引用 → 只发生一次拷贝(xQueueSend 内部那次), 不额外吃栈。
     *  加新载荷类型时用这个, 不必再往 UiBridge 里加方法。 */
    void post_resp_msg(const resp_msg_t &m);

    /*! 投一条流式消息 (覆盖式, 永不失败)。可在 WS 回调里调用。 */
    void post_stream(stream_kind_t kind, const char *text);

    /* ---------------- 消费者 (LVGL 侧) ---------------- */

    /*! 注册 UI 消费定时器 (lv_timer, 周期 50ms)。
     *  ★ 必须在持有 lvgl_port_lock() 的情况下调用。
     *  本阶段消费端只打日志, 不接具体 UI 控件。 */
    esp_err_t start_ui_timer(void);

    /*! 把两通道队列里的东西取出来处理 (本阶段只 log)。
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

    bool push_voice_cmd(voice_cmd_t cmd, int32_t arg); 

    QueueHandle_t m_voice_q    = nullptr;
    QueueHandle_t m_resp_q   = nullptr;
    QueueHandle_t m_stream_q = nullptr;

    RtAsr m_asr;   /*!< 全局常驻实例 (坑#4) */
};
