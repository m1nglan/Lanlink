#include "business/ui_bridge.hpp"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"           /* on_ws_error / on_abroad 要解析 JSON */

#include "drivers/ws.hpp"
#include "lvgl_port.hpp"     /* lvgl_port_set_encoder_enabled: 录音期间禁止转屏 */

#include "display/chat_view.hpp"   /* ★ 气泡的创建/更新/销毁全搬到这里了 */
#include "business/ask_router.hpp" /* ★ "何时问 openclaw"这条策略搬到这里了 */
#include "lvgl.h"            /* lv_timer_t / lv_timer_create */

static const char *TAG = "ui_bridge";

/*! UI 消费周期。流式消息再高频, 也只在"队列非空(有新内容)"时才刷。 */
#define UI_TIMER_PERIOD_MS   (50)

UiBridge& UiBridge::get(void)
{
    static UiBridge s_inst;   /* C++11 起局部 static 初始化线程安全 */
    return s_inst;
}

esp_err_t UiBridge::init(void)
{
    if (m_voice_q != nullptr) {
        return ESP_ERR_INVALID_STATE;   /* 重复 init */
    }

    m_voice_q  = xQueueCreate(VOICE_Q_LEN,  sizeof(voice_cmd_msg_t));
    m_resp_q   = xQueueCreate(RESP_Q_LEN,   sizeof(resp_msg_t));
    m_stream_q = xQueueCreate(STREAM_Q_LEN, sizeof(stream_msg_t));
    if (m_voice_q == nullptr || m_resp_q == nullptr || m_stream_q == nullptr) {
        ESP_LOGE(TAG, "队列创建失败 (voice=%p resp=%p stream=%p)",
                 m_voice_q, m_resp_q, m_stream_q);
        return ESP_ERR_NO_MEM;
    }

    /* asr 绑到 WS 单例 + 注册结果回调。attach 只往 WS 的 handler 表登记, 与建连无关。
     * ★ 重连**不需要**重新 attach: WS::deinit() 不清 handler 表,
     *   且 set_handler() 按 type 自带去重。 */
    m_asr.attach(WS::get());
    m_asr.set_result_callback(&UiBridge::on_asr_result, this);

    /* ★ LLM: 和 asr 一个套路 —— 绑 WS + 注册回调 (回调在 WS 消息分发上下文里跑)。
     *  Llm 驱动会把 partial 的增量在【驱动侧】累积好再回调, 所以这里拿到的永远是完整回复。 */
    m_llm.attach(WS::get());
    m_llm.set_stream_callback(&UiBridge::on_llm_stream, this);

    /* ★ 服务器的 svc_ok → 投 VOICE_SVC_ACKED, 唤醒在等"切服务完成"的 voice_session */
    WS::get().set_svc_ok_callback(&UiBridge::on_svc_ok, this);

    /* ★ 网关错误也要接住 —— 否则"音频超过 55 秒"只会打条日志, voice_task 还在白发音频 */
    WS::get().set_handler("error", &UiBridge::on_ws_error, this);

    /* ★ 网关的"会话已结束"通知 —— 讯飞长时间无声时会自己关掉识别会话,
     *   那时网关不会再下发 final, 板子却还在傻采音。见 on_abroad。 */
    WS::get().set_handler("abroad", &UiBridge::on_abroad, this);

    /* ★ 把"怎么问"的三个动作交给 ask_router —— "什么时候问"那条策略全在它那里。 */
    static const ask_router::hooks_t kAskHooks = {
        &UiBridge::ar_switch_service,
        &UiBridge::ar_send_chat,
        &UiBridge::ar_show_waiting,
    };
    ask_router::init(&kAskHooks);

    ESP_LOGI(TAG, "就绪: voice_q=%d resp_q=%d stream_q=%d (共约 %u 字节内部 SRAM)",
             VOICE_Q_LEN, RESP_Q_LEN, STREAM_Q_LEN,
             (unsigned)(VOICE_Q_LEN * sizeof(voice_cmd_msg_t)
                        + RESP_Q_LEN * sizeof(resp_msg_t)
                        + STREAM_Q_LEN * sizeof(stream_msg_t)));
    return ESP_OK;
}

/* ======================= 投递 (全部非阻塞) ======================= */

bool UiBridge::push_voice_cmd(voice_cmd_t cmd, int32_t arg) 
{
    if (m_voice_q == nullptr) {
        return false;
    }
    voice_cmd_msg_t m = {};
    m.cmd = cmd;
    m.arg = arg;
    return xQueueSend(m_voice_q, &m, 0) == pdTRUE;   /* timeout=0: 绝不阻塞 */
}

bool UiBridge::voice_start(void)
{
    /* ★ 先清掉旧命令再投 START —— 建一条不变式: "START 之后 voice_q 里只有本轮的命令"。
     * ⚠️ 不这么做: on_ws_error 投的 VOICE_STOP 若恰好在用户松手那刻到达, 会残留到下一轮
     *    → voice_q_drain 把它当成"用户松手" → 新一轮【刚按就被取消】。
     * ★ timeout=0: 只清已有的, 绝不阻塞 (本函数可能从 esp_timer 任务调用)。 */
    if (m_voice_q != nullptr) {
        voice_cmd_msg_t junk;
        while (xQueueReceive(m_voice_q, &junk, 0) == pdTRUE) {
            ESP_LOGD(TAG, "voice_start 前清掉旧命令 cmd=%d", (int)junk.cmd);
        }
    }

    bool ok = push_voice_cmd(VOICE_START, 0);
    if (!ok) {
        ESP_LOGW(TAG, "voice_q 满, VOICE_START 丢弃");
    }
    return ok;
}

bool UiBridge::voice_stop(void)
{
    bool ok = push_voice_cmd(VOICE_STOP, 0);
    if (!ok) {
        ESP_LOGW(TAG, "voice_q 满, VOICE_STOP 丢弃");
    }
    return ok;
}

/* ============ resp_q 的**唯一入队点** ============
 * post_resp / post_resp_status 最后都走这里 → "非阻塞发送"和"满了告警"只写一份。
 * 传 const 引用 → 只有 xQueueSend 内部那一次拷贝, 不额外吃栈。 */
void UiBridge::post_resp_msg(const resp_msg_t &m)
{
    if (m_resp_q == nullptr) {
        return;
    }
    /* ★ timeout=0 是硬要求: 本函数会在 WS 回调里被调用, 那时正持 client->lock。
     *   若这里阻塞, 整个 WS 收发(含 voice_task 发音频)都会被拖死。 */
    if (xQueueSend(m_resp_q, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "resp_q 满, 丢弃 kind=%d", (int)m.kind);
    }
}

/* 文本类: 标签 + 字符串 (通用短提示)。
 * ★ 只有 64 字节 (≈21 汉字) —— 识别结果/LLM 回复请走 post_stream。
 * ★ 当前没有调用者 (RESP_STATUS 是预留), 留着是因为 64 × 16 = 1KB 的代价可以接受。 */
void UiBridge::post_resp(resp_kind_t kind, const char *text)
{
    resp_msg_t m = {};
    m.kind = kind;
    m.t_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (text != nullptr) {
        strlcpy(m.u.text, text, sizeof(m.u.text));   /* ★ 绝不用 strcpy: 防截断越界 */
    }
    post_resp_msg(m);
}

/* 状态类: 标签 + status_kind_t (与 post_resp 同形, 只是载荷是枚举而不是文本) */
void UiBridge::post_resp_status(resp_kind_t kind, status_kind_t status)
{
    /* ★ 顺带维护"本轮是否在录音"(两个用途: on_ws_error 的守卫 + 录音期间禁转屏) */
    if (kind == RESP_ASR_STATUS) {
        m_asr_active = (status == STARTED);

        /* ★ 录音期间掐掉编码器输入 → LVGL 不发 LV_KEY_LEFT/RIGHT → 转屏被禁。
         *   lvgl_port_set_encoder_enabled 会【丢弃】暂停期间转的格数, 不会攒到恢复时倾泻。
         *   本函数在 voice_task(CPU0), 标志位在 lvgl 任务(CPU1) 被读 —— 只是 bool, 安全。 */
        lvgl_port_set_encoder_enabled(!m_asr_active);
    }

    resp_msg_t m = {};
    m.kind  = kind;
    m.t_ms  = (uint32_t)(esp_timer_get_time() / 1000);
    m.u.sta = status;
    post_resp_msg(m);
}

void UiBridge::post_stream(stream_kind_t kind, const char *text, bool is_final)
{
    if (m_stream_q == nullptr) {
        return;
    }
    stream_msg_t m = {};
    m.kind     = kind;
    m.is_final = is_final;              /* ★ V2: "流式"和"完成"合并到一条消息 */
    if (text != nullptr) {
        /* ★ text 是【完整文本】(V2) → 整体替换, 没有累积。2048 字节能装 680 汉字。 */
        strlcpy(m.text, text, sizeof(m.text));
    }
    /* xQueueOverwrite: 深度 1 覆盖式 → 永不阻塞、永不失败, 只留最新那句 */
    xQueueOverwrite(m_stream_q, &m);
}

/* ======================= 接收 (LVGL 侧) ======================= */

/* ASR 结果回调: 跑在 websocket_task 且持 client->lock → 只做非阻塞操作。
 * ★ V2: text 已经是【完整句】(网关发的就是全量), 本函数不做任何累积。 */
void UiBridge::on_asr_result(const char *text, bool is_final, void *ctx)
{
    UiBridge *self = static_cast<UiBridge *>(ctx);
    if (self == nullptr || text == nullptr) {
        return;
    }
    /* 上屏: 全走 stream_q (覆盖式, 只留最新) */
    self->post_stream(STREAM_ASR, text, is_final);

    /* ★ final 到了 → 交给 ask_router 决定要不要转发 (它内部有"一轮只问一次"的闸
     *   和"final 不来就超时兜底"那套逻辑, 全在那个模块里)。 */
    ask_router::on_asr(text, is_final);
}

/* LLM 流式回调: 同样跑在 websocket_task 且持锁。
 * ★ 驱动侧 (Llm::handle_partial) 已经把增量累积成【完整回复】, 这里直接投递。 */
void UiBridge::on_llm_stream(const char *text, bool is_final, void *ctx)
{
    UiBridge *self = static_cast<UiBridge *>(ctx);
    if (self == nullptr || text == nullptr) {
        return;
    }
    self->post_stream(STREAM_LLM, text, is_final);
}

/* ---------------- ask_router 的三个注入回调 ----------------
 * ★ "什么时候问"这条策略全在 ask_router 里; 这三个只负责"怎么问"。
 * ★ 全部非阻塞 —— 可能在 websocket_task 且持 client->lock 的上下文里被调用。 */

esp_err_t UiBridge::ar_switch_service(const char *service)
{
    WS::get().set_service(service);                   /* 本地记录, 供诊断 */
    return UiBridge::get().m_llm.switch_service(service, 0);   /* {"type":"svc",...} */
}

esp_err_t UiBridge::ar_send_chat(const char *content)
{
    return UiBridge::get().m_llm.chat(content, 0);    /* {"type":"chat","content":...} */
}

void UiBridge::ar_show_waiting(const char *text)
{
    /* ★ 走 post_stream(非阻塞)而不是直接碰 LVGL: 这条会在 websocket_task
     *   且持着 client->lock 的上下文里被调用。 */
    UiBridge::get().post_stream(STREAM_LLM, text, false);
}

/* svc_ok 回调: 跑在 websocket_task 且持 client->lock → 只做非阻塞投递, 绝不阻塞。
 * 两件事:
 *   ① 切到转发目标服务的确认到了 → ask_router 会把暂存的那句话真正发出去
 *   ② 切到 text 的确认 → 唤醒在等的 voice_session */
void UiBridge::on_svc_ok(const char *service, void *ctx)
{
    UiBridge *self = static_cast<UiBridge *>(ctx);
    if (self == nullptr) {
        return;
    }

    /* ★ 先交给 ask_router 认领 —— 服务名 / 占位文字 / "等 svc_ok 再发 chat"那套逻辑
     *   全在那个模块里, 本文件不再认识 "openclaw" 这个名字。 */
    if (ask_router::handle_svc_ok(service)) {
        return;
    }

    /* 剩下的就是 "text": voice_session 在等这个 */
    self->push_voice_cmd(VOICE_SVC_ACKED, 0);
}

/* 网关错误回调: 跑在 websocket_task 且持 client->lock → 只做非阻塞投递。
 * 目的: 网关判"这一轮不要了"时让 voice_task 立刻收尾, 而不是继续白发音频。
 * 走现成的 VOICE_STOP 通道 → 采音循环接住它并走正常收尾, 服务器 final 还能回来。
 * ⚠️ 必须用 m_asr_active 守卫: 不在录音时投的 STOP 会残留, 被下一轮开头的
 *    voice_q_drain 当成"用户松手"而取消 (voice_start() 里也先清队列, 两道防线)。 */
void UiBridge::on_ws_error(const char *payload, int len, void *ctx)
{
    (void)len;
    UiBridge *self = static_cast<UiBridge *>(ctx);
    if (self == nullptr) {
        return;
    }

    /* code: 1=讯飞错误 2=llm/openclaw 调用失败 3=音频超过55s 4=未知服务 */
    int code = -1;
    cJSON *root = cJSON_Parse(payload);
    if (root != NULL) {
        const cJSON *c = cJSON_GetObjectItem(root, "code");
        if (c != NULL && cJSON_IsNumber(c)) {
            code = c->valueint;
        }
        cJSON_Delete(root);
    }

    /* 1(讯飞错误) / 3(超时) → 这一轮已经废了, 立刻收尾。
     * 2(llm 失败) / 4(未知服务) 与本轮录音无关 → 只记日志。 */
    if (code == 1 || code == 3) {
        if (self->m_asr_active) {
            ESP_LOGW(TAG, "网关报错 code=%d, 本轮录音就此收尾", code);
            self->voice_stop();
        } else {
            ESP_LOGW(TAG, "网关报错 code=%d (当前没在录音, 忽略)", code);
        }
    }
}

/* 网关的"本轮会话已结束"通知: {"type":"abroad","text":<字符串或 null>}
 *
 * ★★ 为什么需要它: 讯飞在【长时间没有音频/没有有效语音】时会自己把识别会话关掉。
 *    那一刻起网关不会再下发 final, 而板子毫不知情 —— 用户按着键继续说话,
 *    音频全发进了空处, 屏幕上永远等不到结果。
 *    有了这个字段, 网关可以主动说"这轮完了", 板子立刻收尾, 不用干等 50 秒上限。
 *
 * ★★ 收到就【立刻结束本轮】—— 不管 text 是什么, 也不管用户松没松开。
 *    (早期写成"只有 null 才结束", 那是错的: 带文本时用户还在按着,
 *     会话其实已经死了, 他继续说的每一句都进不了识别。现在一律收尾。)
 *
 * ★ text 的两种取值:
 *    null   → 没识别到任何内容, 直接收尾
 *    "..."  → 已识别的内容一起带出来(可能是一整段话) → 先上屏再收尾
 *             ⚠️ 上屏必须走 stream_q(STREAM_ABROAD): resp_q 的 u.text 只有 64 字节
 *                (≈21 汉字), 一整段话会被截掉大半。
 *
 * ★ 收尾走 VOICE_STOP → 采音循环接住后走【正常收尾】(mic.stop + asr.end + ENDED),
 *   不是 abort —— final 还能回来。
 * ★ 必须用 m_asr_active 守卫: 不在录音时投的 STOP 会残留,
 *   被下一轮开头的 voice_q_drain 当成"用户松手"而取消新会话 (同 on_ws_error)。 */
void UiBridge::on_abroad(const char *payload, int len, void *ctx)
{
    (void)len;
    UiBridge *self = static_cast<UiBridge *>(ctx);
    if (self == nullptr) {
        return;
    }

    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        ESP_LOGW(TAG, "abroad: JSON 解析失败, 忽略");
        return;
    }

    /* ① 有内容就先上屏 —— 必须在收尾【之前】投出去,
     *    否则气泡可能已被收掉, 这段文字会撞上"已收尾不再新建气泡"被丢。
     *    (post_stream 内部立刻 strlcpy 拷贝, 所以现在 root 还活着, 安全) */
    const cJSON *t = cJSON_GetObjectItem(root, "text");
    const bool   has_text = cJSON_IsString(t) && t->valuestring != NULL
                            && t->valuestring[0] != '\0';
    if (has_text) {
        ESP_LOGW(TAG, "网关通知会话结束, 带出识别内容 %u 字节, 转上屏",
                 (unsigned)strlen(t->valuestring));
        self->post_stream(STREAM_ABROAD, t->valuestring, true);
    }
    cJSON_Delete(root);

    /* ② 无条件收尾: 会话已经结束了, 用户再按着也没用 */
    if (self->m_asr_active) {
        ESP_LOGW(TAG, "网关通知会话已结束 (abroad, %s), 本轮立刻收尾",
                 has_text ? "带内容" : "无内容");
        self->voice_stop();
    } else {
        ESP_LOGI(TAG, "网关通知会话已结束 (abroad) —— 当前没在录音, 忽略");
    }
}

void UiBridge::drain_queues(void)   //< LVGL 侧把两个队列取空 (见 MD/PROTOCOL.md)
{
    /* ★ stream_msg_t 是 2056 字节, 而本函数跑在 lvgl 任务 (栈 6K, 且 lv_timer_handler
     *   内部还会嵌套调用) → 放 static, 绝不能放栈上。
     *   本函数只被 ui_timer_cb 调用(单线程上下文), 所以 static 安全。 */
    static stream_msg_t s;
    static resp_msg_t   r;

    /* ★★ 每轮都同步屏幕状态 (不是只在上屏时): 同步里会删掉 SquareLine 预置的占位气泡,
     *   放到这里 (~137ms 一轮) 就等于"一进聊天屏就清干净", 而不用等用户说第一句话。
     *   幂等, 重复调用只花一次 lv_obj_is_valid 遍历。 */
    chat_view::sync_screen();

    /* ★ 宽限期检查: 松手后等 final 落进气泡, 超时就真收尾
     *   (这样"按了没说"的空气泡不会一直挂着)。见 CHAT_END_GRACE_MS 的说明。 */
    chat_view::tick();

    /* ★ "问转发目标"的兜底: final 迟迟不来时, 用最后一条 asr 顶上。
     *   必须排在下面两个 while 之前 —— 因为 STARTED 会重置 ask_router 的状态。 */
    ask_router::tick();

    /* ---- ① 控制通道先处理 (状态) ----
     * ★★ 顺序很重要: 必须先处理 resp_q 的状态, 再处理 stream_q 的文字。
     *    否则 STARTED 和第一个 asr 落在同一轮时, 文字先到而 s_asr_active 还是 false
     *    → 那个气泡会被判成"已收尾"直接丢掉, 然后 STARTED 才建了个空气泡。
     *    ("状态先于内容" —— 状态决定气泡存不存在, 内容才往里填。) */
    while (xQueueReceive(m_resp_q, &r, 0) == pdTRUE) {
        if (r.kind == RESP_ASR_STATUS) {
            /* ★★ 用【会话状态】驱动气泡生命周期 —— 按下键就立刻有气泡,
             *    不用等服务器回第一个字。来源: voice_task 的 post_resp_status。 */
            if (r.u.sta == STARTED) {
                /* ★ 顺序: 先让 ask_router 把上一轮还没问出去的冲出去 + 重置状态,
                 *   再让聊天视图收尾上一条 + 放空气泡。chat_view 不知道 ask 的存在。 */
                ask_router::on_session_start();
                chat_view::begin_asr();      /* 按下 → 收尾上一条 + 放一个空气泡 */
            } else {
                /* ENDED / ABORTED → 只记时刻, 不立刻收尾:
                 * 服务器的 final 是网络回包(~400ms 后才到), 得留个落点,
                 * 否则它到了没处放 → 又新建一个气泡。真正收尾在 chat_view::tick。 */
                chat_view::end_asr();
                /* ★ 紧接着让 ask_router 起"等 final"的兜底表 —— 它要知道
                 *   "这一轮说完了", 才能决定 2.5 秒后要不要用最后一条 asr 顶上。 */
                ask_router::on_session_end();
            }
            ESP_LOGI(TAG, "[会话] status=%d", (int)r.u.sta);
        } else if (r.kind == RESP_WS_STATUS) {
            ESP_LOGI(TAG, "[连接] t=%ums status=%d", (unsigned)r.t_ms, (int)r.u.sta);
        } else if (r.kind == RESP_STATUS) {
            /* 通用短提示 (≤21 汉字)。**目前没有生产者** —— abroad 的文本可能是一整段,
             * 已经改走 stream_q(STREAM_ABROAD) 了。这条分支留着是为阶段 4 的弹窗/提示条,
             * 到那时 post_resp(RESP_STATUS, "…") 就有调用者了。
             * ⚠️ 必须单独一个分支 —— 掉到下面的 else 会被当成识别结果打成 [结果]。 */
            ESP_LOGI(TAG, "[提示] %s", r.u.text);
        } else {
            ESP_LOGI(TAG, "[结果] kind=%d t=%ums text=\"%s\"",
                     (int)r.kind, (unsigned)r.t_ms, r.u.text);
        }
    }

    /* ---- ② 流式通道后处理 (内容) ----
     * ★★ V2 的核心收益: s.text 永远是【完整文本】→ 这里【零累积状态】,
     *    收到什么就显示什么, 不需要判断"该替换还是该追加"。 */
    while (xQueueReceive(m_stream_q, &s, 0) == pdTRUE) {
        switch (s.kind) {
        case STREAM_ASR:                        /* 我的话: 绿底 / 靠右 */
            /* ★ 诊断用: final=1 说明服务器下发了【最终修正版】(通常只差个句号),
             *   它和 final=0 在屏幕上走的是同一条路。排查"屏幕和网关日志不一样"时,
             *   在日志里搜 `定格` 或 `final=1` 就知道 final 到底到没到板子。
             *   (流式的那些用 DEBUG 级, 免得每 137ms 刷一行) */
            if (s.is_final) {
                ESP_LOGI(TAG, "[流式] ASR final=1 text=\"%s\"", s.text);
            } else {
                ESP_LOGD(TAG, "[流式] ASR final=0 text=\"%s\"", s.text);
            }
            /* ★ 记下最后一条 —— ask_router 的超时兜底用的就是它。
             *   在这里(LVGL 任务)更新而不是在 WS 回调里, 是为了和它的 tick 同上下文, 无竞争。 */
            ask_router::remember_last_asr(s.text);
            chat_view::show_asr(s.text, s.is_final);
            break;

        case STREAM_ABROAD:
            /* 网关的 {"type":"abroad"} 带出来的识别内容 —— 会话已结束, 这是"最后认到的"。
             * ★ 内容和 STREAM_ASR 一样是【我的话】→ 走同一条显示路径 (绿底/靠右)。
             * ★ 也记进 ask_router: 它就是最后一条识别结果, 超时兜底该用它。 */
            ESP_LOGW(TAG, "[流式] ABROAD text=\"%s\"", s.text);
            ask_router::remember_last_asr(s.text);
            chat_view::show_asr(s.text, true);
            break;

        case STREAM_LLM:                        /* 对方回复: 灰底 / 靠左 */
            chat_view::show_llm(s.text, s.is_final);
            break;
        default:
            ESP_LOGW(TAG, "[流式] 未知 kind=%d, 丢弃", (int)s.kind);
            break;
        }
    }
}

/* lv_timer 回调。★ 由 lv_timer_handler() 调用, 此时 LVGL 锁已被持有,
 * 所以只能调**不带锁**的内部函数, 绝不能碰 lvgl_port_lock()。 */
static void ui_timer_cb(lv_timer_t *t)
{
    (void)t;
    UiBridge::get().drain_queues();
}

esp_err_t UiBridge::start_ui_timer(void)
{
    if (lv_timer_create(ui_timer_cb, UI_TIMER_PERIOD_MS, nullptr) == nullptr) {
        ESP_LOGE(TAG, "lv_timer_create 失败");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UI 消费 timer 已注册 (每 %dms 取一次队列)", UI_TIMER_PERIOD_MS);
    return ESP_OK;
}
