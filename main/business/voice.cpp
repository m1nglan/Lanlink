#include "business/voice.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "business/ui_bridge.hpp"
#include "drivers/button_edge.hpp"
#include "drivers/i2s_mic.hpp"
#include "drivers/ws.hpp"

static const char *TAG = "voice";

/* ---------------- 任务参数 ----------------
 * ★ 核选择: 必须与 websocket_task 同核(CPU0) —— 发音频要抢 client->lock。 */
#define VOICE_TASK_STACK     (6 * 1024)
#define VOICE_TASK_PRIO      (6)        /*!< 高于 ws_keeper(5): 发音频不能被保活拖住 */
#define VOICE_TASK_CORE      (0)

#define VOICE_WAIT_CONN_MS   (15000)    /*!< 会话开始前等 WS 建连的超时 */
#define VOICE_SVC_WAIT_MS    (300)      /*!< 等服务器回 svc_ok 的上限。
                                         *   局域网往返通常 <50ms; 300ms 是原来盲等的猜测值,
                                         *   若日志出现"等 svc_ok 超时"再调大 */

/*! ★ 单轮录音时长上限 —— 主动收尾, 不等网关报错。
 *
 *  网关协议 (APIserver.md §2) 明写: "单轮最长 55 秒", 超出会回
 *  {"type":"error","code":3,"message":"音频超过55s"}。
 *
 *  为什么要在【我们这边】也设一个:
 *    - 到点后网关已经停止识别了, 我们再发音频全是白费(还占 WiFi 带宽)
 *    - 网关的计数起点是它收到 start 的时刻, 可能比我们早几十 ms →
 *      卡在 55000 有踩线风险
 *  所以取 50 秒, 留 5 秒余量。
 *
 *  ★ 到点后走的是【正常收尾路径】(mic.stop + asr.end + ENDED), 不是 abort ——
 *    这样服务器那边的 final 还能正常回来, 用户拿到的是完整识别结果。
 *  ★ 兜底: 万一网关比我们早判超时, 它的 {"type":"error"} 会让
 *    UiBridge::on_ws_error 投一个 VOICE_STOP, 采音循环同样会收尾(见 ui_bridge.cpp)。 */
#define VOICE_MAX_RECORD_MS  (50000)

/* ---------------- 音量标定 (为"服务器无响应"判定做准备) ----------------
 * 思路: "收不到识别结果"有两种原因, 现象一样但含义相反 ——
 *         ① 用户没说话       → 服务器本来就不会回字   ← 正常, 不能判死
 *         ② 服务器死了/半开  → 有声音也永远不回字     ← 该判死
 *       用【本帧音量】把两者分开: 只有"有声音却没结果"才说明服务器有问题。
 *
 * 标定步骤:
 *   1. 保持 VOICE_LEVEL_LOG_MS=1000, 烧录, 按 IO10 分别试"不说话"和"正常说话"
 *   2. 看日志 `voice: level=xxx` —— 安静通常在几十~几百, 正常说话几千以上
 *   3. 把 VOICE_SPEECH_LEVEL 设成两者中间的值
 *   ⚠️ 已知残余风险: **持续的环境噪声**(风扇/空调/远处人声)也会超过阈值,
 *      但服务器识别不出字 → 仍可能误判。所以阈值别定太低。 */
#define VOICE_LEVEL_LOG_MS   (1000)     /*!< 每秒打印一次当前音量 (0 = 关闭) */

/*! ★ 说话音量阈值 (一帧平均绝对幅度, 0~32767)。
 *  标定前是**占位值**: 本版本只用它打印日志, 判死逻辑还没接线(见文件末尾)。
 *  按上面的步骤量出真实数值后, 改这里 + 打开那段判死代码。 */
#define VOICE_SPEECH_LEVEL   (32767)

/* ---------------- 服务器无响应判定 (待标定后启用) ----------------
 * 逻辑(尚未接线, 等上面的阈值定下来再打开):
 *
 *   int level = I2sMic::frame_level(pcm, sizeof(pcm)/2);
 *   if (level >= VOICE_SPEECH_LEVEL) {
 *       if (speech_since == 0) speech_since = now_ms();     // 刚开始有声音
 *       // "有声音"已持续 N 秒, 但这段时间服务器一个字都没回 → 它死了
 *       if (now_ms() - speech_since > VOICE_RX_SILENCE_MS &&
 *           ub.asr_last_ms() < speech_since) {
 *           aborted = true; break;
 *       }
 *   } else {
 *       speech_since = 0;                                   // 安静了, 重新计
 *   }
 *
 * 还需要: UiBridge 记一个 "最近一次收到 partial/final 的时刻" (asr_last_ms())。
 * ⚠️ 不能用 ws.is_stale() 代替 —— 它的阈值是 130 秒(要大于服务器 120s ping),
 *    对十几秒的语音会话永远不触发。 */
#define VOICE_RX_SILENCE_MS  (5000)     /*!< "有声音"后服务器沉默多久判死 */

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* 把 voice_q 里已有的命令清掉。
 * 返回 true  = 队列已清空, 且期间没发现 STOP (可以继续)
 * 返回 false = 期间发现了 STOP (用户已松手) → 本轮该取消
 *
 * ★★ 为什么需要它: voice_session 里有几个"等待某件事"的循环 (等连接最长 15 秒),
 *   期间 voice_task 是 voice_q **唯一的消费者**。一不消费队列就会:
 *     ① 深度只有 4 (VOICE_Q_LEN), 按几次就满 → 后续按键被 xQueueSend(...,0) 丢弃
 *     ② 最要命的是 VOICE_STOP 也一起丢 → 用户松手被忽略
 *        → 连上之后照常录一段用户根本不要的会话, 一直录到下次按了又松
 *   实测踩过: 开机 2.4 秒(还没连上)时打出 `voice_q 满, VOICE_STOP 丢弃`。 */
static bool voice_q_drain(QueueHandle_t q, voice_cmd_msg_t *out)
{
    bool got_stop = false;
    while (xQueueReceive(q, out, 0) == pdTRUE) {   /* timeout=0: 只清已有的, 不等待 */
        if (out->cmd == VOICE_STOP) {
            got_stop = true;           /* 记下, 但继续把残渣清完 */
        } else {
            ESP_LOGD(TAG, "丢弃命令 cmd=%d", (int)out->cmd);
        }
    }
    return !got_stop;
}

/* 等服务器确认服务切换完成。
 * 信号来源: ws.cpp 收到 svc_ok → 回调 UiBridge::on_svc_ok → 投 VOICE_SVC_ACKED。
 * 返回:  1 = 收到确认
 *        0 = 超时
 *       -1 = 期间用户松手了(快速点按) → 本轮该取消
 * ★ 必须一边等一边看 STOP: 用户可能按下就松, 若只等 svc_ok 会漏掉松手信号,
 *   然后开始一段用户早就不想录的会话。 */
static int wait_svc_ok(QueueHandle_t q, voice_cmd_msg_t *out)   //< task阻塞在这个函数 超时前等待队列的切换消息
{ 
    uint32_t t0 = now_ms();
    while (now_ms() - t0 < VOICE_SVC_WAIT_MS) {
        if (xQueueReceive(q, out, pdMS_TO_TICKS(20)) != pdTRUE) {
            continue;
        }
        if (out->cmd == VOICE_SVC_ACKED) {
            return 1;
        }
        if (out->cmd == VOICE_STOP) {
            return -1;
        }
        /* 其它命令(不该出现)忽略 */
    }
    return 0;
}

/* ================================================================
 * 一轮语音会话 (真采音)
 *
 * 流程: 等连接 → 切 text → asr.start → mic.start → 采音循环 → mic.stop → asr.end
 *
 * ★ 三个必须守住的边界 (注释里都标了 ★):
 *   ① 读失败时 i2s_mic 在格式转换【之前】就 return 了, pcm 里还是上一帧旧数据
 *      → 绝不能发, 否则等于重复发同一段音频
 *   ② STOP / 断线检查必须在 continue 之前 → 否则麦克风故障时会永远结束不了会话
 *   ③ 发送阻塞会吃 DMA 余量: dma_desc_num(8) x dma_frame_num(256) = 2048 采样 = 128ms
 * ================================================================ */
static void voice_session(UiBridge &ub, I2sMic &mic)   //< 执行函数
{
    WS             &ws      = WS::get();
    RtAsr          &asr     = ub.asr();
    QueueHandle_t   voice_q = ub.voice_queue();
    voice_cmd_msg_t m;

    ESP_LOGI(TAG, "=== 会话开始 ===");
    ub.post_resp_status(RESP_ASR_STATUS, STARTED);

    /* ① 等连接就绪 (连接生命周期归 ws_keeper, 这里只等)
     * ★ 这个循环最长跑 15 秒, 期间**必须也消费 voice_q** —— 否则用户在这段时间里
     *   按/松按键会把深度 4 的队列塞满, 连 VOICE_STOP 都被丢弃 → 用户松手被忽略。
     *   详见 voice_q_drain() 的说明。 */
    uint32_t t0 = now_ms();
    while (!ws.is_connected()) {   //< 等WS连上网关
        if (!voice_q_drain(voice_q, &m)) {
            ESP_LOGI(TAG, "等连接期间用户已松手 (快速点按), 本轮取消");
            ub.post_resp_status(RESP_ASR_STATUS, ENDED);
            return;
        }
        if (now_ms() - t0 > VOICE_WAIT_CONN_MS) {
            ESP_LOGW(TAG, "等连接超时, 会话中止");
            ub.post_resp_status(RESP_ASR_STATUS, ABORTED);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* ② 切到 text 服务 —— ★ 两句必须成对:
     *      ws.set_service     = 本地路由 (收到的 partial 分给哪个槽位)
     *      asr.switch_service = 通知服务器
     *    漏掉前者的后果是静默的: partial 被送去 chat 槽位(阶段2 是 NULL),
     *    ws.cpp 里 `if (h != NULL)` 不报错 → 识别文字凭空消失。
     *
     * ★ 先清掉队列里可能残留的 VOICE_SVC_ACKED (上一次会话的确认, 若残留):
     *   不清掉的话我们可能被【上一次的确认】提前唤醒 → 又变成"没切好就发 start"。
     *   ⚠️ 但如果清的过程中发现 STOP, 说明用户已经松手(快速点按) → 本轮直接取消,
     *      绝不能把它也丢掉, 否则会话会一直录到下次按松。 */
    if (!voice_q_drain(voice_q, &m)) {   //< 清空队列(内部判 STOP)
        ESP_LOGI(TAG, "会话刚开始用户已松手 (快速点按), 本轮取消");
        ub.post_resp_status(RESP_ASR_STATUS, ENDED);
        return;
    }

    ws.set_service("text");
    asr.switch_service("text", WS_SEND_TIMEOUT_MS);

    /* ★ 等到服务器**真的**确认切好, 才往下走 (原来是无条件 vTaskDelay(300),
     *   服务器慢一点就变成"在旧服务上发 start", 而那种错是静默的) */
    #pragma region   //< 发switch:text 阻塞等网关切换
    int sw = wait_svc_ok(voice_q, &m);  
    if (sw == -1) {
        ESP_LOGI(TAG, "等 svc_ok 期间用户已松手 (快速点按), 本轮取消");
        ub.post_resp_status(RESP_ASR_STATUS, ENDED);
        return;
    }
    if (sw == 0) {
        ESP_LOGW(TAG, "等 svc_ok 超时 (%dms), 会话中止", VOICE_SVC_WAIT_MS);
        ub.post_resp_status(RESP_ASR_STATUS, ABORTED);
        return;
    }
    ESP_LOGI(TAG, "服务器已确认切到 text");
    #pragma endregion

    #pragma region   //< 发start 开麦克风
    /* ③ 开始一轮识别 */
    if (asr.start(WS_SEND_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGE(TAG, "start 发送失败, 会话中止");
        ub.post_resp_status(RESP_ASR_STATUS, ABORTED);
        return;
    }
    ESP_LOGI(TAG, "已发送 start");

    /* ④ 开麦克风 —— ★ 必须放在 asr.start() 之后:
     *    麦克风一 enable, DMA 立刻开始填数据; 服务器还没准备好就白发前几帧 */
    if (mic.start() != ESP_OK) {
        ESP_LOGE(TAG, "麦克风启动失败, 会话中止");
        asr.end(WS_SEND_TIMEOUT_MS);            /* 会话已开, 补一个 end 收场 */
        ub.post_resp_status(RESP_ASR_STATUS, ABORTED);
        return;
    }
    ESP_LOGI(TAG, "采音开始 (每帧 %d 字节 / %dms)", I2S_MIC_FRAME_BYTES, I2S_MIC_FRAME_MS);
    #pragma endregion


    /* ⑤ 采音循环: 读一帧 → 发一帧
     *   ★ pcm 放 static: 1280 字节不要压在本任务 6K 的栈上 */
    static int16_t pcm[I2S_MIC_FRAME_BYTES / 2];
    bool     aborted   = false;
    uint32_t frames    = 0;   //< 成功发送的帧数
    uint32_t read_fail = 0;   //< 读失败被 continue 跳过的帧数 (= 被丢掉的音频)
    uint32_t t_rec     = now_ms();   //< 采音起点 (算单轮时长用)

#if VOICE_LEVEL_LOG_MS > 0
    uint32_t t_level     = now_ms();
    uint32_t last_frames = 0;   //< 上一秒的 frames 快照
    uint32_t last_fails  = 0;   //< 上一秒的 read_fail 快照
    uint32_t read_ms_sum = 0;   //< 本秒 read_frame 累计耗时 (判"有没有余量")
#endif

    while (1) {
#if VOICE_LEVEL_LOG_MS > 0
        uint32_t t_read = now_ms();
#endif
        esp_err_t r = mic.read_frame(pcm, sizeof(pcm), I2S_MIC_FRAME_MS);   //< 收音
#if VOICE_LEVEL_LOG_MS > 0
        read_ms_sum += now_ms() - t_read;
#endif

        /* ★ 边界②: 不管读没读到, 都先看 STOP / 断线 */
        if (xQueueReceive(voice_q, &m, 0) == pdTRUE && m.cmd == VOICE_STOP) {
            break;                              /* 用户松开 → 正常结束 */
        }
        /* ★ 边界③: 单轮时长上限 (网关规定最长 55 秒, 我们 50 秒主动收尾)。
         *   走【正常收尾】而不是 abort: 后面的 mic.stop + asr.end 照常执行,
         *   服务器的 final 还能回来, 用户拿到的仍是完整识别结果。
         *   (不是 aborted —— 只是"录满了", 不是出错。) */
        if (now_ms() - t_rec >= VOICE_MAX_RECORD_MS) {
            ESP_LOGW(TAG, "已达单轮上限 %u 秒, 自动收尾 (网关规定最长 55 秒)",
                     (unsigned)(VOICE_MAX_RECORD_MS / 1000));
            break;
        }
        if (!ws.is_connected()) {
            ESP_LOGW(TAG, "采音期间连接断开, 会话中止");
            aborted = true;
            break;
        }

        /* ★ 边界①: 读失败时 pcm 还是上一帧旧数据 → 既不能发包, 也不能算音量。
         *   但这**等于丢掉这 40ms 音频** —— 以前这里是完全静默的, 串口一个字都没有,
         *   于是"发出去的音频千疮百孔、服务器识别不出东西"这种情况完全看不见。
         *   现在前 1 次 + 每 25 次打一条。 */
        if (r != ESP_OK) {
            read_fail++;
            if (read_fail == 1 || (read_fail % 25) == 0) {
                ESP_LOGW(TAG, "读帧失败 %u 次 (r=%s) —— 已丢弃 %u 帧音频",
                         (unsigned)read_fail, esp_err_to_name(r), (unsigned)read_fail);
            }
        }

#if VOICE_LEVEL_LOG_MS > 0
        /* 每秒汇总 —— ★ 位置很关键: 必须在【读失败判断之外】。
         *   否则"读一直在失败"时这条日志一条都打不出来, 正好把最该看的情况藏住了。 */
        if (now_ms() - t_level >= VOICE_LEVEL_LOG_MS) {
            t_level = now_ms();
            uint32_t sent = frames    - last_frames;
            uint32_t fail = read_fail - last_fails;
            last_frames = frames;
            last_fails  = read_fail;

            /* 四个数字判读:
             *   发送帧   —— 满帧应为 1000/40 = 25/秒。**明显偏少 = 正在丢音频**
             *               (整个循环 >40ms/帧 → DMA 缓冲积压 → 绕着圈覆盖 → 音频有洞)
             *   读失败帧 —— 每多 1 就是 40ms 音频被直接扔掉
             *   读耗时   —— read_frame 在【等音频】上花了多少。这是"还有多少余量"的度量:
             *               生产恒定 40ms/帧, 本循环每轮耗时 read + send。
             *               读≈35ms → 发送只要 ~5ms, 余量充足 ✓
             *               读≈0ms  → 发送吃掉 ~40ms, 刚好卡平, 一点抖动就积压 ⚠️
             *   发送帧 <25 且 读≈0 → 确认在积压丢音频
             *   GPIO中断 —— 正常按一次只涨几次~几十; 几百/几千 = 引脚在噪声里翻转。 */
            uint32_t avg_read = sent ? (read_ms_sum / sent) : 0;
            read_ms_sum = 0;
            ESP_LOGI(TAG, "level=%d | 本秒 发送=%u 读失败=%u 帧 (满帧应为 %d), 读耗时均=%ums | GPIO中断累计=%u",
                     (r == ESP_OK) ? I2sMic::frame_level(pcm, sizeof(pcm) / sizeof(int16_t)) : -1,
                     (unsigned)sent, (unsigned)fail,
                     1000 / I2S_MIC_FRAME_MS, (unsigned)avg_read,
                     (unsigned)button_edge_isr_count());
        }
#endif

        if (r != ESP_OK) {
            continue;                   /* 读失败: 绝不发(那是上一帧的旧数据) */
        }

        if (asr.send_audio((const uint8_t *)pcm, sizeof(pcm), WS_SEND_TIMEOUT_MS) != ESP_OK) {  //< 发包
            ESP_LOGE(TAG, "音频发送失败, 会话中止");
            aborted = true;
            break;
        }
        frames++;
    }

    /* ⑥ 收尾 —— ★ mic.stop() 在所有退出路径上都必须执行,
     *    否则 I2S 通道一直开着空转, 下轮 mic.start() 也会因已运行而失败 */
    mic.stop();
    asr.end(WS_SEND_TIMEOUT_MS);

    /* 总耗时 = frames x 40ms (读成功并发出的) ; 丢帧数直接告诉你有多少音频没上去。
     * 判读: 按住 N 秒 → 满帧应为 N*25; 若 共发 明显偏少 而 读失败 很大,
     *       就是"音频千疮百孔" → 服务器自然识别不出东西。 */
    ESP_LOGI(TAG, "会话结束 (%s), 共发 %u 帧 (=%.1f 秒音频), 读失败丢弃 %u 帧 (=%.1f 秒)",
             aborted ? "中止" : "正常",
             (unsigned)frames, frames * (float)I2S_MIC_FRAME_MS / 1000.0f,
             (unsigned)read_fail, read_fail * (float)I2S_MIC_FRAME_MS / 1000.0f);

    ub.post_resp_status(RESP_ASR_STATUS, aborted ? ABORTED : ENDED);
    /* 注意: 这里**不等 final** —— final 由 websocket_task 收到后经 asr 回调
     *       投进 resp_q, 由 UI 侧消费。本任务直接回队列睡觉。 */
}

/* ================================================================
 * 常驻任务: 阻塞在命令队列上
 * ================================================================ */
static void voice_task(void *arg)
{
    (void)arg;
    UiBridge        &ub      = UiBridge::get();
    QueueHandle_t    voice_q = ub.voice_queue();
    voice_cmd_msg_t  m;

    /* ---- 麦克风: 建通道只做一次 (i2s_new_channel + 配置较重), start/stop 每轮会话做。
     *  ★ 全机唯一采音者就是本任务 —— i2s_mic.cpp 的读缓冲是 static, 只能有一个读者。 */
    I2sMic  mic;
    if (mic.init() != ESP_OK) {    //< 待办-开机自检
        /* 不退出任务: 让后面的会话照常走, 在 mic.start() 处失败并报 ABORTED,
         * 这样 UI 至少能收到"中止"信号, 而不是整条语音功能无声消失 */
        ESP_LOGE(TAG, "麦克风初始化失败 —— 后续会话会直接中止");
    }

    while (1) {    //< switch判消息 只关注VOICE_START
        /* ★ 空闲时睡在这里, 零 CPU 占用。由 xQueueSend 唤醒, 不是轮询。 */
        if (xQueueReceive(voice_q, &m, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (m.cmd) {
        case VOICE_START:
            voice_session(ub, mic);
            break;

        case VOICE_STOP:
            /* 会话外收到 STOP: 说明是快速点按后残留的, 忽略即可 */
            ESP_LOGI(TAG, "空闲状态收到 VOICE_STOP, 忽略");
            break;

        case VOICE_SVC_ACKED:
            /* 会话外收到的服务切换确认 —— ws_keeper 重连时也会切服务,
             * 服务器同样回 svc_ok。没人等它, 忽略。 */
            ESP_LOGD(TAG, "空闲状态收到切换确认 cmd=%d, 忽略", (int)m.cmd);
            break;

        default:
            ESP_LOGW(TAG, "未知命令 cmd=%d", (int)m.cmd);
            break;
        }
    }
}

esp_err_t voice_task_start(void)   //< 启动voice_task
{
    BaseType_t ok = xTaskCreatePinnedToCore(voice_task, "voice",
                                            VOICE_TASK_STACK, nullptr,
                                            VOICE_TASK_PRIO, nullptr,
                                            VOICE_TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "voice_task 创建失败");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "voice_task 已启动 (CPU%d prio%d 栈%d)",
             VOICE_TASK_CORE, VOICE_TASK_PRIO, VOICE_TASK_STACK);
    return ESP_OK;
}
