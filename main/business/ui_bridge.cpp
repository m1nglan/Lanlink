#include "business/ui_bridge.hpp"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"           /* on_ws_error / on_abroad 要解析 JSON */

#include "drivers/ws.hpp"
#include "drivers/weather.hpp"      /* 天气: 解析 + 最新快照 */
#include "lvgl_port.hpp"     /* lvgl_port_set_encoder_enabled: 录音期间禁止转屏 */

#include "display/chat_view.hpp"   /* ★ 气泡的创建/更新/销毁全搬到这里了 */
#include "display/cho_view.hpp"     /* ★ 选择模型面板 (开合/焦点/选中项) */
#include "display/msg_ball.hpp"     /* ★ 主界面的"有新消息"提示球 */
#include "display/msg_panel.hpp"    /* ★ 主界面的"通知展开面板" */
#include "display/server_view.hpp"  /* ★ Server 屏: 服务器监控上屏 */
#include "display/balance_view.hpp" /* ★ Balance 屏: 余额 */
#include "display/weather_view.hpp"/* ★ 天气上屏 (kind→图, temp→数字, text→文字) */
#include "business/ask_router.hpp" /* ★ "何时问 openclaw"这条策略搬到这里了 */
#include "lvgl.h"            /* lv_timer_t / lv_timer_create */

static const char *TAG = "ui_bridge";

/*! UI 消费周期。流式消息再高频, 也只在"队列非空(有新内容)"时才刷。 */
#define UI_TIMER_PERIOD_MS   (50)

/*! ★ 停在 Server 屏多久才切 svc=usage (防连滑)。
 *  编码器一格约 1/4 圈, 人正常翻屏不会在一屏停 1 秒 → 路过的不会触发 ✓ */
#define SERVER_DWELL_MS      (1000)

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

    /* ★ 天气: 网关在 svc weather 之后会自动推一次 {"type":"weather",...}。
     *   ★★ 注意 svc 是【全局独占】的 —— 天气轮询会切走服务, 所以它必须判空闲
     *      (见 weather_poll), 否则会把正在进行的语音/对话掐了。
     *   这里只负责"收到就存下来", 上屏在 drain_queues 里做。 */
    weather::init();
    WS::get().set_handler("weather", &UiBridge::on_weather, this);

    /* ★ 服务器监控: 切到 usage 服务后网关每 2 秒自动推一条 {"type":"usage",...}
     *   载荷塞在 resp_msg_t.u.usage (42 字节, 不超原来 64 字节的 union) ✓ */
    WS::get().set_handler("usage", &UiBridge::on_usage, this);

    /* ★ 余额: 切到 balance 服务后网关推一次 (不像 usage 那样定时推 ✗),
     *   之后想刷新要主动发 {"type":"get"}。这里只负责收。 */
    WS::get().set_handler("balance", &UiBridge::on_balance, this);

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
    } else {
        /* ★ 立刻置"即将开始": 从这一刻到 voice_task 切完 svc text 之间有一个窗口,
         *   那期间 m_asr_active 还是 false —— 不让天气轮询趁虚而入抢走服务。
         *   见 m_pending_start 的说明。 */
        m_pending_start = true;
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

/*! 投一条物理按键原始事件。★ 只报告"哪个键/按下还是松开", 不判断语义。
 *  载荷用 u.i32 打包: (key << 1) | pressed —— 不扩 resp_msg_t, 零内存增长。 */
void UiBridge::post_key(int key, bool pressed)
{
    resp_msg_t m = {};
    m.kind = RESP_KEY;
    m.t_ms = (uint32_t)(esp_timer_get_time() / 1000);
    m.u.i32 = (int32_t)(((uint32_t)key << 1) | (pressed ? 1u : 0u));
    post_resp_msg(m);
}

/* 状态类: 标签 + status_kind_t (与 post_resp 同形, 只是载荷是枚举而不是文本) */void UiBridge::post_resp_status(resp_kind_t kind, status_kind_t status)
{
    /* ★ 顺带维护"本轮是否在录音"(两个用途: on_ws_error 的守卫 + 录音期间禁转屏) */
    if (kind == RESP_ASR_STATUS) {
        m_asr_active = (status == STARTED);
        /* ★ STARTED 一到就清掉"即将开始"标志 —— 守卫交给 m_asr_active 接管。
         *   ENDED/ABORTED 时也清一遍, 防残留 (比如 voice_task 在切服务阶段就取消了)。 */
        m_pending_start = false;

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

    /* ★★ 只有 "text" 的确认才是 voice_session 在等的那个 (投 VOICE_SVC_ACKED)。
     *   以前这里没判断服务名 → 任何"别人要的" svc_ok 都会被当成 text 的确认 ✗
     *   现在多了天气轮询 (会切 svc weather), 后果很严重:
     *     voice_task 会以为"text 切好了"就发 start, 而网关其实还在 weather 服务上
     *     → 那一轮语音静默失效 (没有报错, 只是永远收不到识别结果)。
     *   ⇒ 必须按服务名过滤。 */
    if (strcmp(service, "text") != 0) {
        ESP_LOGD(TAG, "svc_ok(%s) 不是本机在等的, 忽略", service);
        return;
    }
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

void UiBridge::on_weather(const char *payload, int len, void *ctx)
{
    (void)ctx;
    /* ★ 只解析 + 存快照。天气数据不经过队列 —— 本模块靠一个"脏标记"交接给
     *   LVGL 任务 (见 weather.hpp 顶部的说明: 低频数据不值得再开一条队列)。
     *   跑在 websocket_task 且持 client->lock → 内部只做 cJSON 解析 + 一次 memcpy。 */
    weather::handle(payload, len);
}

void UiBridge::on_usage(const char *payload, int len, void *ctx)
{
    (void)ctx;
    /* ★ 跑在 websocket_task 且持 client->lock → 只解析 + 投队列, 不碰 LVGL。
     *   ★ 每 2 秒一条, 投 resp_q (可靠但会挤) —— 解析失败直接丢, 下一轮 2 秒后又来 ✓ */
    cJSON *root = cJSON_ParseWithLength(payload, (size_t)len);
    if (root == NULL) {
        ESP_LOGW(TAG, "usage: JSON 解析失败, 忽略");
        return;
    }

    resp_msg_t m = {};
    m.kind = RESP_USAGE;
    m.t_ms = (uint32_t)(esp_timer_get_time() / 1000);

    /* 网关把 ram_used/disk_used 发成字符串, 其余是整数 */
    cJSON *v = NULL;
    v = cJSON_GetObjectItem(root, "cpu");       m.u.usage.cpu      = (int16_t)(v ? v->valueint : 0);
    v = cJSON_GetObjectItem(root, "ram_pct");   m.u.usage.ram_pct  = (int16_t)(v ? v->valueint : 0);
    v = cJSON_GetObjectItem(root, "disk_pct");  m.u.usage.disk_pct = (int16_t)(v ? v->valueint : 0);
    v = cJSON_GetObjectItem(root, "rx");        m.u.usage.rx       = (int16_t)(v ? v->valueint : 0);
    v = cJSON_GetObjectItem(root, "tx");        m.u.usage.tx       = (int16_t)(v ? v->valueint : 0);

    v = cJSON_GetObjectItem(root, "ram_used");
    if (cJSON_IsString(v) && v->valuestring != NULL) {
        strlcpy(m.u.usage.ram_used, v->valuestring, sizeof(m.u.usage.ram_used));
    }
    v = cJSON_GetObjectItem(root, "disk_used");
    if (cJSON_IsString(v) && v->valuestring != NULL) {
        strlcpy(m.u.usage.disk_used, v->valuestring, sizeof(m.u.usage.disk_used));
    }
    /* ★ CPU 负载文字, 网关字段名是 cpu_str, 形如 "0.3/2 Core"。
     *   用字符串是为了让网关自己决定格式 (负载/核心数/线程数), 板子只负责原样显示 ✓ */
    v = cJSON_GetObjectItem(root, "cpu_str");
    if (cJSON_IsString(v) && v->valuestring != NULL) {
        strlcpy(m.u.usage.cpu_str, v->valuestring, sizeof(m.u.usage.cpu_str));
    }
    cJSON_Delete(root);

    get().post_resp_msg(m);      /* ★ 静态成员没法直接拿到单例 → 用 get() */
}

void UiBridge::on_balance(const char *payload, int len, void *ctx)
{
    (void)ctx;
    /* ★ 跑在 websocket_task 且持 client->lock → 只解析 + 投队列, 不碰 LVGL。
     *   报文: {"type":"balance","balance":true,"total":11.96,"t":1792411591}
     *   ⚠️ total 是【浮点】number, 不是字符串 ✗ 用 valuedouble 取。 */
    cJSON *root = cJSON_ParseWithLength(payload, (size_t)len);
    if (root == NULL) {
        ESP_LOGW(TAG, "balance: JSON 解析失败, 忽略");
        return;
    }
    cJSON *v = cJSON_GetObjectItem(root, "total");
    if (!cJSON_IsNumber(v)) {
        ESP_LOGW(TAG, "balance: 没有 total 字段, 忽略");
        cJSON_Delete(root);
        return;
    }
    const double total = v->valuedouble;
    cJSON_Delete(root);

    resp_msg_t m = {};
    m.kind = RESP_BALANCE;
    m.t_ms = (uint32_t)(esp_timer_get_time() / 1000);
    m.u.f32 = (float)total;
    get().post_resp_msg(m);
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
        } else if (r.kind == RESP_USAGE) {
            /* 服务器监控 —— 载荷在 u.usage 里, 直接交给上屏模块 */
            server_view::apply(&r.u.usage);
        } else if (r.kind == RESP_BALANCE) {
            /* 余额 —— 载荷在 u.f32 里 */
            balance_view::apply(r.u.f32);
        } else if (r.kind == RESP_KEY) {
            /* ★★★ 物理按键的【语义判断】全在这里 —— 按键回调只负责报告按了哪个键。
             *
             * 用户定的规则:
             *   · 面板关着 → KEY_B(IO42) 就是【录音键】: 按下开始 / 松开结束
             *   · 面板开着 → KEY_B(IO42) 是【取消】, 且面板期间【录音禁用】;
             *                KEY_A(IO2) 是【确认】
             *   · 面板关着 + 在 chat 屏 → KEY_A(IO2) 打开面板
             *
             * ★ 两者互斥 (不可能同时是取消又是录音), 所以不需要长按检测 ✓
             * ★ "如果当前焦点处不能用取消键, 就默认它是录音" —— 正是下面
             *    `if (cho_view::is_open())` 这个判据: 没面板就用不了取消 → 落到录音分支。 */
            const int  key     = (int)((uint32_t)r.u.i32 >> 1);
            const bool pressed = ((r.u.i32 & 1) != 0);

            if (key == KEY_A) {
                /* ---- IO2: 确认 / 打开面板 ----
                 * ★ 优先级: 模型面板开着 → 确认; 通知面板开着 → 进 chat 屏;
                 *   都没开 → 在 chat 屏开模型面板, 在 home 屏开通知面板。 */
                if (!pressed) {
                    /* 只在按下时动作 (松开不做事, 否则会"确认两次") */
                } else if (cho_view::is_open()) {
                    /* ---- 确认: 焦点项 = 新选中的模型 ---- */
                    const int sel = cho_view::focused_choice();
                    cho_view::set_selected(sel);
                    cho_view::close();

                    /* ★ 两个图标 → 网关的两个服务。
                     *   协议完全相同 (chat/partial/reply), 换的只是服务名。
                     *   ★ set_service 只改"目标", 不马上发 svc —— 真正切换发生在下次
                     *     提问时 (switch_service + 等 svc_ok + chat), 所以不抢 svc 状态。
                     *   ⚠️ 若此刻有提问正在切服务, set_service 会拒绝 (返回 false);
                     *     那种情况极少 (要面板开着 + 刚好在等 svc_ok), 只打告警。 */
                    const char *svc = (sel == cho_view::CHOICE_DEEP) ? "llm" : "openclaw";
                    const char *name = (sel == cho_view::CHOICE_DEEP) ? "DeepSeek" : "明岚";
                    if (ask_router::set_service(svc)) {
                        ESP_LOGI(TAG, "[按键] 确认模型: %s (转发服务 → %s)", name, svc);
                        /* ★ 只清屏幕上的气泡, **不发 {"type":"clear"}**。
                         *   原因: 网关的服务上下文是【按服务各存一份】的 ——
                         *   切到 llm / openclaw 本身就是换了一套上下文, 不需要额外清 ✗
                         *   而且 clear 还会让网关回一条"上下文已清空"的确认,
                         *   那条会走 reply → show_llm → 屏幕上冒个多余气泡 ✗ */
                        chat_view::clear_all();
                    } else {
                        ESP_LOGW(TAG, "[按键] 确认模型: %s 未生效 (有提问在流程中, 请稍后再选)", name);
                    }
                } else if (msg_panel::is_open()) {
                    /* ---- 通知面板开着 → 点按 ----
                     * ★ 有消息: 进 chat 屏; 没消息 (只显示"没有消息~"): 只是收起 ✗
                     *   不加这个判断的话, 空面板点一下会直接跳 chat ✗ (踩过) */
                    if (msg_panel::has_entry()) {
                        ESP_LOGI(TAG, "[按键] 从通知面板进入聊天页 (%s)",
                                 (msg_panel::focused_entry() == msg_panel::ENTRY_CLAW) ? "明岚" : "DeepSeek");
                        msg_panel::enter_chat();
                    } else {
                        msg_panel::close();
                        ESP_LOGI(TAG, "[按键] 通知面板里没有消息 → 收起");
                    }
                } else if (cho_view::can_open()) {
                    cho_view::open();
                    ESP_LOGI(TAG, "[按键] 打开模型面板");
                } else if (msg_panel::can_open()) {
                    msg_panel::open();
                    ESP_LOGI(TAG, "[按键] 打开通知面板");
                } else if (balance_view::is_active()) {
                    /* ---- Balance 屏: IO2 = 刷新余额 ----
                     * ★ 先把标签打成占位符 "--.--", 再发 {"type":"get"} 让网关重查。
                     *   顺序很重要: 先占位再请求, 用户点下去立刻有反馈 ✓
                     * ★ 网关回 balance 后, drain_queues → balance_view::apply() 会写回真实数字。
                     *   begin_refresh() 内部已经作废了"上次值"缓存, 所以即使余额没变
                     *   也一定会写回去 ✓ (否则标签会永远停在 "--.--" ✗) */
                    balance_view::begin_refresh();
                    if (WS::get().is_connected()) {
                        WS::get().send_text("{\"type\":\"get\"}", 0);
                        ESP_LOGI(TAG, "[按键] 刷新余额 (已发 get)");
                    } else {
                        ESP_LOGW(TAG, "[按键] 刷新余额: WS 未连接, 只打了占位符");
                    }
                } else {
                    ESP_LOGD(TAG, "[按键] IO2 在当前屏无动作");
                }
            } else if (key == KEY_B) {
                /* ---- IO42: 录音 / 取消 ----
                 * ★ 优先级: 通知面板开着 → 收起它 (不录音); 模型面板开着 → 取消;
                 *   都没开 → 正常录音键。 */
                if (msg_panel::is_open()) {
                    if (pressed) {
                        msg_panel::close();
                        ESP_LOGI(TAG, "[按键] 收起通知面板");
                    }
                    return;
                }
                if (cho_view::is_open()) {
                    /* 面板开着 → 录音禁用, 这个键就是纯【取消】 */
                    if (pressed) {
                        cho_view::close();
                        ESP_LOGI(TAG, "[按键] 取消面板 (选择不变)");
                    }
                } else {
                    /* 面板没开 → 正常录音键 (按下开始 / 松开结束) */
                    if (pressed) {
                        ESP_LOGI(TAG, "[按键] 录音键按下 → voice_start");
                        voice_start();          /* drain_queues 是成员函数, 隐式 this */
                    } else {
                        ESP_LOGI(TAG, "[按键] 录音键松开 → voice_stop");
                        voice_stop();
                    }
                }
            }
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
            /* ★ 收到【第一条流式文字】就亮"有新消息"提示球 —— 不等 final,
             *   因为 openclaw 可能憋十几秒才吐第一个字, 等 final 太晚了。
             *   ★ 后续每条 partial 都会再调一次, 但 notify_incoming 内部靠
             *     一个 s_unread 标志判重 → 只操作控件一次, 不会闪 ✓
             * ★ 放在这里而不是 WS 回调里: 那里持着 client->lock, 不能碰 LVGL。 */
            msg_ball::notify_incoming();
            /* ★ 同时把正文实时写进"通知面板"里对应的那条 (流式替换)。
             *   哪一条由当前转发目标决定 —— 和"点确认切模型"保持同步。
             *   ⚠️ 判断放这里而不是 msg_panel 内部: display 层不该反向依赖
             *      business 层的 ask_router ✓ */
            msg_panel::set_reply_text(s.text,
                                      (strcmp(ask_router::get_service(), "openclaw") == 0));
            break;
        default:
            ESP_LOGW(TAG, "[流式] 未知 kind=%d, 丢弃", (int)s.kind);
            break;
        }
    }

    /* ---- ③ 天气 (没有队列, 靠脏标记交接) ----
     * ★ 为什么放在两个 while 之后: 天气和会话内容完全无关, 顺序无所谓;
     *   放最后是为了不打断"状态先于内容"那条铁律的阅读顺序。
     * ★ take_dirty() 读到就清 → 同一份数据不会重复上屏 (weather::handle 内部还会
     *   做"值没变不置脏"的判断, 两道保险)。 */
    if (weather::take_dirty()) {
        weather::snapshot_t w{};
        weather::get(&w);
        if (w.valid) {
            weather_view::apply(&w.info);
        }
    }

    /* ---- ④ "有新消息"提示球 ----
     * 只看一个 bool + 一次屏幕比较, 每轮 50ms 跑一次也无所谓。
     * ★ 必须放在两个 while 之后: 同一轮里如果先收到回复 (notify_incoming) 再
     *   update, 而用户此刻刚好在 chat 屏 —— notify 那边已经跳过了, update 也不会
     *   误清; 顺序反过来同样安全, 所以这里只需保证"每轮都跑一次"。 */
    msg_ball::update();

    /* ---- ⑤ 通知面板的"已读" ----
     * 幂等: 内部自己判断"是否在 chat 屏", 不在就直接 return。
     * 所以这里无条件调, ui_bridge 不用认识 ui_chat 这种屏符号 ✓
     * ★ 只要进了 chat 屏就标记已读: 不管是点通知面板进的、还是转编码器进的 ✓ */
    msg_panel::mark_read();

    /* ---- ⑥ Server / Balance 屏的服务切换 (带防连滑) ----
     * ★ 为什么要防: 网关只在对应服务下才推数据。但编码器把屏一格格滑过去时,
     *   如果每次路过都发一条 svc, 会一路狂切服务 (还打断语音/对话) ✗
     * ★ 做法: 在 Server/Balance 屏【连续停留 SERVER_DWELL_MS】才切过去; 一离开立刻切回 text。
     *   ⚠️ 从一个"有服务"的屏直接滑到另一个 (Server → Balance) 时, 目标变了要重新计时,
     *     否则会立刻切过去 (dwell 早就满了) ✗
     * ⚠️ 这里用 drain_queues 的 50ms 节拍累加, 不引入新定时器。 */
    {
        static uint32_t dwell_ms = 0;
        static int      active_id = 0;    /* 0=无, 1=usage, 2=balance */
        static int      last_want = 0;

        /* 目标: 在哪个屏就该用哪个服务 */
        const char *want = nullptr;
        int         want_id = 0;
        if (server_view::is_active()) {
            want = "usage";   want_id = 1;
        } else if (balance_view::is_active()) {
            want = "balance"; want_id = 2;
        }

        if (want_id != last_want) {
            dwell_ms  = 0;                /* ★ 目标变了 → 重新计时 */
            last_want = want_id;
        }

        if (want_id != 0) {
            if (dwell_ms < SERVER_DWELL_MS) {
                dwell_ms += UI_TIMER_PERIOD_MS;
            }
            if (active_id != want_id && dwell_ms >= SERVER_DWELL_MS && WS::get().is_connected()) {
                char msg[48];
                snprintf(msg, sizeof(msg), "{\"type\":\"svc\",\"service\":\"%s\"}", want);
                WS::get().set_service(want);
                if (WS::get().send_text(msg, 0) == ESP_OK) {
                    active_id = want_id;
                    ESP_LOGI(TAG, "[屏] 停留 %ums → 切 svc %s", (unsigned)dwell_ms, want);
                }
            }
        } else {
            dwell_ms = 0;
            if (active_id != 0) {
                /* ★ 离开立刻切回 text —— 否则下次语音会跑到 usage/balance 上 ✗
                 *   ⚠️ 正在录音时不抢: voice_task 自己会切 text ✓ */
                if (!is_asr_active() && WS::get().is_connected()) {
                    WS::get().set_service("text");
                    WS::get().send_text("{\"type\":\"svc\",\"service\":\"text\"}", 0);
                }
                active_id = 0;
                ESP_LOGI(TAG, "[屏] 离开 → 切回 svc text");
            }
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
