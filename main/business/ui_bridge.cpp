#include "business/ui_bridge.hpp"

#include <string.h>
#include <stdio.h>           /* snprintf: 给发给 ASK_SERVICE 的文字拼前缀 */

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"   /* heap_caps_malloc: 明确要求 PSRAM */
#include "cJSON.h"           /* on_ws_error 要解析 {"type":"error","code":N} */

#include "drivers/ws.hpp"
#include "lvgl_port.hpp"     /* lvgl_port_set_encoder_enabled: 录音期间禁止转屏 */

#include "lvgl.h"
#include "ui.h"              /* ui_contextpanel / ui_font_ch14 (INCLUDE_DIRS 里有 "lvgl") */

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

/* ---- "问 ASK_SERVICE" 的触发状态 ----
 * ★★ 必须定义在这里 (on_asr_result 之前) —— C 语言要求先声明后使用,
 *    放后面会报 'was not declared in this scope'。
 * 读写上下文: 只有 s_final_seen / s_asked 是 WS 回调写, 其余全在 LVGL 任务里。 */

/*! 收到过 final 吗? 用来取消兜底 (final 一到就没必要再等) */
static volatile bool s_final_seen = false;
/*! 本轮【已经问过】ASK_SERVICE 了吗 —— 三条触发路径 (final / 超时兜底 / 下一轮冲出去)
 *  共用这一道闸, 保证一轮只问一次。
 *  ⚠️ 检查与置位不是原子的 (WS 回调 + LVGL 任务都可能写), 但窗口只有几微秒,
 *     且最坏后果是"同一个问题问两次", 不值得为它上临界区。 */
static volatile bool s_asked = false;
/*! 最近一条 ASR 文本 (final=0 也更新), 兜底时拿它去问 */
static char     s_last_asr[512];
/*! ENDED 已到、还在等 final (兜底表已起) */
static bool     s_ask_armed = false;
/*! 兜底触发的时刻 (ms) */
static uint32_t s_ask_deadline = 0;

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

    /* ★★ final 到了 → 立刻转发给 openclaw (这是最准的触发点: 它是整句的最终版)。
     *  ⚠️ 但不能【只】靠它 —— 网关的 end 处理还没修好时 final 永远不来,
     *     那样就永远不会去问 openclaw, 灰泡功能整个失效。
     *     所以还会在 ENDED 之后起一个超时兜底 (见 ask_tick / ASK_FINAL_WAIT_MS)。
     *  s_asked 是"本轮只问一次"的闸: 若兜底已经抢先用最后一条 asr 问过了
     *  (final 迟到了 2.5 秒以上), 这里就不再问, 免得冒出两个灰泡。 */
    if (is_final) {
        s_final_seen = true;
        if (!s_asked) {
            s_asked = true;
            self->llm_ask(text);
        }
    }
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

/*! 语音识别结果转发给哪个服务。
 *  "openclaw" = 明岚   |   "llm" = DeepSeek
 *  ★ 两者协议完全相同 (chat / partial / reply), 换的只是服务名 ——
 *    想切回来只改这一行。 */
#define ASK_SERVICE  "openclaw"

/*! 发给 ASK_SERVICE 的提问前缀 —— 让明岚一眼看出这话是从 Lanlink 设备来的。
 *  实际发出的是 {"type":"chat","content":"[Lanlink] <识别文字>"}。
 *  ★ 尾巴上带个空格, 免得和第一个字粘在一起。 */
#define ASK_PREFIX   "[Lanlink] "

/*! 问题已发出、等回复期间占位用的灰泡文字。
 *  ★ 明岚调工具时实测要 3~25 秒 (多在 20 秒上下) 才吐第一个字, 这段空窗期
 *    屏幕上什么都没有, 用户会以为死机了。
 *  ★ 怎么"消失"的: 它就是一条普通的 STREAM_LLM 文本, 第一条 partial/reply
 *    到达时 chat_view_show_llm 会把 s_live_res 的文字整个覆盖掉 —— 自动替换,
 *    不需要任何删除逻辑。 */
#define ASK_WAIT_TEXT  "(思考中)"

/*! 松手后最多等 final 多久 (ms), 超了就用最后一条 asr 兜底去问 openclaw。
 *
 * ★★ 为什么需要兜底: 触发转发靠的是 final, 而 final 是网关【收到 end 之后】才发的。
 *    网关的 end 处理一旦有问题(实测就是), final 永远不来 —— 那样就永远不问 openclaw,
 *    灰泡功能整个失效, 而且从现象上看像"openclaw 坏了", 极难定位。
 *    有了兜底: final 来了用 final(最准), 不来就用最后一条 asr(通常只差个句号)。
 * ★ 取 2.5 秒: final 实测在 end 之后 ~400ms 到, 给足余量又不至于让用户干等。 */
#define ASK_FINAL_WAIT_MS  (2500)

/* 待发的提问 —— llm_ask 先切服务, 等 on_svc_ok 到了再真正发 chat。
 *
 * ★★ 为什么必须等 svc_ok（实测过, 见 tools/gw_probe.mjs / gw_probe2.mjs）:
 *    网关和板子都在切换过程中, 背靠背发 chat 有静默失败的风险 ——
 *    服务不对时 chat 不会被拒绝, 而是被 echo 静默吞掉(探针①验证),
 *    表现是"气泡建了却永远等不到回复", 极难排查。
 *    等 svc_ok 才发, 就和 voice_session 切 text 的写法完全一致。
 *    (svc_ok 实测 31~34ms 就回来, 等得起)
 *
 * ★ 为什么不在 llm_ask 里等: 它在 WS 回调里跑 (websocket_task 正持 client->lock),
 *   阻塞会把整个 WS 收发拖死。所以只能"存下来, 等回调里补发"。 */
static char s_pending_chat[512];    /*!< BUS_TEXT_LEN 是 2048, 但一句话 512 绰绰有余 */
static bool s_pending = false;

/* (本轮"问 ASK_SERVICE"的触发状态 s_final_seen / s_asked / s_last_asr / s_ask_armed /
 *  s_ask_deadline 定义在文件上方 on_asr_result 之前 —— 那里先用。) */

/* 把一句话发给 ASK_SERVICE: 切服务 + 暂存这句话(带 ASK_PREFIX 前缀),
 * 等 svc_ok 回来再发 chat。
 * ★ 不切回 text —— 下一轮录音时 voice_session 开头自己会切回。
 * ★ 用 snprintf 而不是 strlcpy: 要拼前缀, 且 content 过长时它安全截断。 */
bool UiBridge::llm_ask(const char *content)
{
    if (content == nullptr || content[0] == '\0') {
        return false;
    }

    snprintf(s_pending_chat, sizeof(s_pending_chat), ASK_PREFIX "%s", content);
    s_pending = true;

    WS::get().set_service(ASK_SERVICE);                 /* 本地记录, 供诊断 */
    esp_err_t ret = m_llm.switch_service(ASK_SERVICE, 0);   /* {"type":"svc","service":...} */

    ESP_LOGI(TAG, "[LLM] 转发识别结果到 %s (%.24s%s): %s, 等 svc_ok 后发 chat",
             ASK_SERVICE, content, (content[24] != '\0') ? "..." : "", esp_err_to_name(ret));
    return ret == ESP_OK;
}

/* svc_ok 回调: 跑在 websocket_task 且持 client->lock → 只做非阻塞投递, 绝不阻塞。
 * 两件事:
 *   ① 切到 ASK_SERVICE 的确认到了 → 把暂存的那句话真正发出去 (见 llm_ask 的说明)
 *   ② 切到 text 的确认 → 唤醒在等的 voice_session */
void UiBridge::on_svc_ok(const char *service, void *ctx)
{
    UiBridge *self = static_cast<UiBridge *>(ctx);
    if (self == nullptr) {
        return;
    }

    if (strcmp(service, ASK_SERVICE) == 0) {
        if (s_pending) {
            s_pending = false;
            esp_err_t ret = self->m_llm.chat(s_pending_chat, 0);   /* ★ 现在才发 */
            ESP_LOGI(TAG, "[LLM] %s 已就绪, 发 chat (%.24s%s): %s",
                     ASK_SERVICE, s_pending_chat,
                     (s_pending_chat[24] != '\0') ? "..." : "", esp_err_to_name(ret));

            /* ★ 发成功就立刻放一个"(思考中)"的灰泡占位。
             *   接下来十几二十秒明岚都不会吐字, 没这个占位屏幕是空的。
             *   真回复的第一条 partial 到达时会把它整条覆盖掉 —— 见 ASK_WAIT_TEXT 说明。
             *   走 post_stream(非阻塞)是为了不在这里碰 LVGL: 本函数跑在
             *   websocket_task 且持着 client->lock。 */
            if (ret == ESP_OK) {
                self->post_stream(STREAM_LLM, ASK_WAIT_TEXT, false);
            }
        }
        return;   /* 不是 voice_session 在等的那个服务, 别去打扰它 */
    }

    /* service == "text": voice_session 在等这个 */
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
 *    音频全发进了空处, 屏幕上永远等不到结果。以前只能靠 50 秒本地硬上限兜底。
 *    有了这个字段, 网关可以主动说"这轮完了"。
 *
 * ★ text 的两种取值, 走【两条不同的路】:
 *
 *   ① text = null  → 本轮到此为止, 且【没有识别到任何内容】
 *        → 投 VOICE_STOP → 采音循环接住后走【正常收尾】
 *          (mic.stop + asr.end + ENDED, 不是 abort —— final 还能回来)
 *
 *   ② text = "..." → 会话结束了, 但【已识别的内容一起带出来】(可能是一整段话)
 *        → 走 stream_q(STREAM_ABROAD) 上屏
 *        ⚠️ 不能走 resp_q: 它的 u.text 只有 64 字节(≈21 汉字), 一整段话会被截掉大半。
 *        ⚠️ 也【不】结束录音 —— 按用户定的语义, 只有 null 才结束本轮;
 *           带文本是"顺手把已认到的吐出来", 用户可能还在接着说。
 *
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

    const cJSON *t = cJSON_GetObjectItem(root, "text");

    if (t == NULL || cJSON_IsNull(t)) {
        cJSON_Delete(root);
        if (self->m_asr_active) {
            ESP_LOGW(TAG, "网关通知会话已结束 (abroad text=null), 本轮录音立刻收尾");
            self->voice_stop();                 /* 走正常收尾, asr.end 照发 */
        } else {
            ESP_LOGI(TAG, "网关通知会话已结束 (abroad text=null) —— 当前没在录音, 忽略");
        }
        return;
    }

    if (!cJSON_IsString(t) || t->valuestring == NULL) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "abroad: text 既不是 null 也不是字符串, 忽略");
        return;
    }

    if (t->valuestring[0] == '\0') {
        /* 空串按"没内容"处理 —— 和 null 同义, 免得留一条空气泡 */
        cJSON_Delete(root);
        if (self->m_asr_active) {
            ESP_LOGW(TAG, "网关通知会话已结束 (abroad text 为空), 本轮录音收尾");
            self->voice_stop();
        }
        return;
    }

    /* ★ post_stream 内部立刻 strlcpy 拷贝走, 所以在这里(root 还活着)调用是安全的。
     *   is_final=true: 这是网关给的"收尾文本", 该定格。 */
    ESP_LOGW(TAG, "网关通知会话已结束, 带出识别内容 %u 字节, 转上屏",
             (unsigned)strlen(t->valuestring));
    self->post_stream(STREAM_ABROAD, t->valuestring, true);

    cJSON_Delete(root);
}

/* ================================================================
 * 聊天视图: 双向气泡
 *
 * [链] 上行(我说的话):
 *   stream_q(STREAM_ASR) → chat_view_show_asr → chat_push(text, is_me=true, …)
 *                        → chat_add_bubble → chat_label_set
 * [链] 下行(对方回复):
 *   on_asr_result(is_final) → llm_ask → 网关
 *      → Llm::handle_partial/reply (驱动侧累积成完整回复)
 *      → on_llm_stream → stream_q(STREAM_LLM) → chat_view_show_llm
 *                      → chat_push(text, is_me=false, …)
 * [链] 会话状态(管气泡生死):
 *   resp_q → chat_view_begin_asr (STARTED, 按下就出空气泡)
 *          → chat_view_end_asr  (ENDED/ABORTED, 只记时刻)
 *          → chat_view_tick     (宽限期到期才真收尾)
 *   ★ 以上全部由 drain_queues 在 lvgl 任务里驱动, 且先过 chat_view_sync_screen。
 *
 * 控件三层 (属性要与 SquareLine 一致): ui_contextpanel(flex column, 可滚)
 *      → roll(wrapper) → panel(气泡) → label(文字)
 * ★ wrapper 的意义: flex 的交叉轴对齐是**整容器一个值**, 做不到"我的靠右"。
 *   wrapper 是 flex 孩子(x/y/align 失效), panel 的父 wrapper 无 layout → align 生效
 *   (lv_obj_pos.c:779)。详见 HANDOFF §6-F。
 * ================================================================ */

#define CHAT_MAX_BUBBLES  (10)     /*!< 最多留几条消息 (超限删最老的) */

/*! 松开按键后等服务器 final 的宽限期 (ms)。
 *
 *  ★ 为什么要等: 松手后 voice_task 立刻投 ENDED, 而服务器的 final 是【网络回包】,
 *    必然晚到。若在 ENDED 就把气泡收尾, final 到了没有落点,
 *    会又新建一个气泡 —— 一次按键冒两个。所以留一段时间让它落进同一条。
 *
 *  ★★ 为什么是 2500 (实测数据, 别改小):
 *    实测 `end` → `final` = 【1470ms】。原来定 1500ms, 余量只有 30ms ——
 *    网络稍微抖一下, final 就会晚于宽限期到达,
 *    然后撞上 chat_view_show_asr 的"③ 已收尾, 不再新建气泡"被丢掉,
 *    屏幕停在最后一条 asr 上(差个句号), 而日志里 final 明明到了, 极难查。
 *    改成 2500 后余量 ≈ 1030ms。
 *
 *  ★ 与 ASK_FINAL_WAIT_MS 取同一个值: 两者都是"final 该在多久内到"这一件事,
 *    同值可以避免读代码时来回换算。语义也一致 ——
 *    这段时间内 final 到了就用 final, 没到就用最后一条 asr。 */
#define CHAT_END_GRACE_MS (2500)

/* 存 wrapper 指针 —— 删 wrapper 会连带删掉里面的气泡和标签 */
static lv_obj_t *s_bubbles[CHAT_MAX_BUBBLES];
static int       s_bubble_cnt = 0;

/* "当前这一轮"的气泡标签 (流式期间反复改它的文字)。
 * 由 STARTED 建、由【宽限期到期】或下一次 STARTED 收尾 —— 见 chat_view_tick/begin_asr。 */
static lv_obj_t *s_live_label = NULL;

/* "对方"(LLM 回复) 当前正在写的那条气泡。比 ASR 那边简单得多:
 * 一次 chat 只有一条 reply, 所以 is_final 时直接释放即可 (没有多段问题)。 */
static lv_obj_t *s_live_res = NULL;

/* ENDED 的时刻 (esp_timer ms); 0 = 没有待收尾的 */
static uint32_t  s_end_ms = 0;

/* 本轮语音会话是否还在进行 (由 STARTED / ENDED 维护)。
 * 用途: 文字到达时若气泡已被收尾【就不要新建】—— 那正是"冒第二个气泡"的来源。 */
static bool      s_asr_active = false;

/* 上次记录的那些指针属于哪个 contextpanel —— 用来识别"聊天屏被销毁重建了" */
static lv_obj_t *s_ctx_hooked = NULL;

/* ★ 气泡文字要不要显式放 PSRAM?
 *   0 = lv_label_set_text, 让 LVGL 拷到它自己的堆 (内部 SRAM) —— 当前值, 和 SquareLine 一致
 *   1 = heap_caps_malloc(MALLOC_CAP_SPIRAM) + set_text_static (代码仍在, 见 #if 分支)
 *   ⚠️ 当初设 1 时现象是"气泡变长但文字不画", 一度以为这条路坏了 ——
 *      实际根因是 ui_font_ch14 的压缩字体坑 (见 HANDOFF §6-C), 与这条路径无关。
 *   现状: 一条识别结果才 60~120 字节, 10 条约 1~2KB 内部 SRAM, 不值得折腾;
 *        等 LLM 长回复上来 (每条可上千字节) 再打开。 */
#define CHAT_TEXT_IN_PSRAM  (0)

#if CHAT_TEXT_IN_PSRAM
/* 标签被删时释放我们自己分配的 PSRAM 文字副本。
 * ★ set_text_static 之后 LVGL 永不释放文字 (lv_label.c:774) → 归我们管。 */
static void chat_label_deleted_cb(lv_event_t *e)
{
    lv_obj_t *label = (lv_obj_t *)lv_event_get_target(e);
    char *buf = (char *)lv_obj_get_user_data(label);
    if (buf != nullptr) {
        heap_caps_free(buf);              /* PSRAM 和内部堆都能 free */
        lv_obj_set_user_data(label, NULL);
    }
}
#endif

/* 把文字设进标签 (见 CHAT_TEXT_IN_PSRAM 的两个分支说明) */
static void chat_label_set(lv_obj_t *label, const char *text)
{
    if (label == NULL || text == NULL) {
        return;
    }

#if CHAT_TEXT_IN_PSRAM
    size_t n   = strlen(text) + 1;
    char  *buf = (char *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        buf = (char *)heap_caps_malloc(n, MALLOC_CAP_DEFAULT);   /* 退回内部堆 */
        if (buf == NULL) {
            ESP_LOGE(TAG, "气泡文字分配失败 (%u 字节)", (unsigned)n);
            return;
        }
        ESP_LOGW(TAG, "PSRAM 分配失败, 气泡文字退回内部 SRAM (%u 字节)", (unsigned)n);
    }
    memcpy(buf, text, n);

    char *old = (char *)lv_obj_get_user_data(label);
    lv_label_set_text_static(label, buf);   /* LVGL 只存指针, 不拷贝也不释放 */
    lv_obj_set_user_data(label, buf);       /* 所有权交给我们, 记在标签上 */
    if (old != NULL) {
        heap_caps_free(old);                /* 流式刷新时会走到这里 */
    }

    /* ⚠️ 诊断用 (只在 PSRAM 路径下打): 确认标签里到底存了什么。
     *    如果这里打印的字符串是对的、但屏幕还是不画 → 问题在绘制阶段;
     *    如果打印的字符串本身就不对 → 问题在 set_text_static / 指针。 */
    {
        const char *got = lv_label_get_text(label);
        ESP_LOGI(TAG, "[PSRAM路径] 存入 %u 字节 @%p, 标签读回=\"%s\"",
                 (unsigned)n, (void *)buf, got ? got : "(null)");
    }
#else
    lv_label_set_text(label, text);         /* ★ LVGL 自己拷贝 (内部堆) */
#endif
}

/* 新建一个气泡。is_me=true → 我的消息(绿底/靠右); false → 对方回复(灰底/靠左)。
 * 返回文字标签指针。
 *
 * ★ 两层 wrapper 是必要的: LVGL 的 flex 交叉轴对齐是【整容器一个值】, 做不到
 *   "我的靠右、对方的靠左"。所以每条消息套一层 wrapper: wrapper 是 flex 孩子
 *   (自己的 x/y/align 无效), 气泡是 wrapper 的孩子而 wrapper 没有 layout
 *   → 气泡的 align 生效 (lv_obj_pos.c:777 见到父有 layout 就直接 return)。
 * ★ 属性照抄 SquareLine 导出的 ui_mepanel / ui_respanel, 别自己发挥(比如加 radius)。 */
static lv_obj_t *chat_add_bubble(const char *text, bool is_me)
{
    /* ---- 1. wrapper (flex 孩子) ---- */
    lv_obj_t *roll = lv_obj_create(ui_contextpanel);
    lv_obj_set_width(roll, is_me ? 310 : 314);
    lv_obj_set_height(roll, LV_SIZE_CONTENT);
    lv_obj_remove_flag(roll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(roll, 0, 0);
    lv_obj_set_style_bg_opa(roll, 0, 0);         /* 透明 */
    lv_obj_set_style_border_opa(roll, 0, 0);
    lv_obj_set_style_pad_all(roll, 0, 0);
    if (!is_me) {
        lv_obj_set_style_pad_left(roll, 5, 0);   /* 和 ui_resrollpane 一致 */
    }

    /* ---- 2. 气泡 (align 在这里生效) ---- */
    lv_obj_t *panel = lv_obj_create(roll);
    lv_obj_set_size(panel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_align(panel, is_me ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(panel, lv_color_hex(is_me ? 0x35D28D : 0x2F2F30), 0);
    lv_obj_set_style_bg_opa(panel, 255, 0);
    lv_obj_set_style_border_opa(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, 8, 0);
    lv_obj_set_style_pad_right(panel, 8, 0);
    lv_obj_set_style_pad_top(panel, 6, 0);
    lv_obj_set_style_pad_bottom(panel, 6, 0);
    /* ★ 这里【故意不设 radius】—— 和 SquareLine 导出的 ui_mepanel 一致: 样例也没设,
     *   走主题默认 (lv_theme_default.c:27 的 RADIUS_DEFAULT, 随 DPI 缩放)。
     *   自己写死一个数就和 SquareLine 预览不一致了。 */

    /* ---- 3. 文字 (默认已是 LV_LABEL_LONG_WRAP, 见 lv_label.c:748) ---- */
    lv_obj_t *label = lv_label_create(panel);
    lv_obj_set_size(label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(label, 280, 0);
    lv_obj_set_style_text_font(label, &ui_font_ch14, 0);
    /* 我的: 黑字配绿底; 对方: 白字配灰底 (和 ui_restext 一致) */
    lv_obj_set_style_text_color(label, lv_color_hex(is_me ? 0x000000 : 0xFFFFFF), 0);
#if CHAT_TEXT_IN_PSRAM
    lv_obj_add_event_cb(label, chat_label_deleted_cb, LV_EVENT_DELETE, NULL);
#endif
    chat_label_set(label, text);

    return label;
}

/* 新建并登记一条消息, 超限删最老的。
 * ★ why 只用于日志 —— 标出【是哪条路径】建的气泡, 排查"多出一个气泡"时看它就够了。 */
static lv_obj_t *chat_push(const char *text, bool is_me, const char *why)
{
    if (s_bubble_cnt >= CHAT_MAX_BUBBLES) {
        /* 删 wrapper → 连带删气泡和标签 (LVGL 是树形, 删父删子) */
        lv_obj_del(s_bubbles[0]);
        memmove(s_bubbles, s_bubbles + 1,
                sizeof(s_bubbles[0]) * (CHAT_MAX_BUBBLES - 1));
        s_bubble_cnt--;
    }

    lv_obj_t *label = chat_add_bubble(text, is_me);
    lv_obj_t *roll  = lv_obj_get_parent(lv_obj_get_parent(label));   /* label→panel→roll */
    s_bubbles[s_bubble_cnt++] = roll;

    ESP_LOGI(TAG, "[气泡] 新建 #%d %s (原因: %s) text=\"%.24s\"",
             s_bubble_cnt - 1, is_me ? "我" : "对方", why, (text != NULL) ? text : "");

    /* 新气泡的坐标要等布局算完才有 → 先更新布局再滚。
     * (lv_obj_update_layout 重入时 early return, 见 lv_obj_pos.c:383 —— 不会死锁) */
    lv_obj_update_layout(ui_contextpanel);
    lv_obj_scroll_to_view(roll, LV_ANIM_ON);

    return label;
}

/* 上屏前同步屏幕状态。返回 false = 现在不能上屏。
 *
 * ★★ 为什么需要: SquareLine 的聊天屏**离开即销毁** (ui_chat.c:40 的 SCREEN_UNLOADED),
 *   所以 ui_contextpanel 会换新对象, 我们记在 s_bubbles[]/s_live_label 里的指针
 *   全部变悬空 → 下一次 lv_obj_del 就是 use-after-free。
 *   每次换屏都做三件事: ① 不在聊天屏(NULL)就什么都不做
 *   ⚠️ 绝不能拿 NULL 去 lv_obj_create —— 那会创建【一个新的屏幕】而不是子对象。
 *   ② contextpanel 换对象了 → 清空记录 + 删掉 SquareLine 预置的占位气泡
 *   ③ 兜底: 记录里的指针失效了 → 也清空。详见 HANDOFF §6-E。 */
static bool chat_view_sync_screen(void)
{
    if (ui_contextpanel == NULL) {
        /* 还没进过聊天屏, 或已经离开了 → 忘掉一切, 绝不能拿去当父对象 */
        s_bubble_cnt  = 0;
        s_live_label  = NULL;
        s_live_res    = NULL;
        s_ctx_hooked  = NULL;
        s_end_ms      = 0;
        s_asr_active  = false;
        return false;
    }

    if (s_ctx_hooked != ui_contextpanel) {
        /* 屏幕换了 (第一次进, 或离开过又进来) → 旧指针全部作废 */
        if (s_ctx_hooked != NULL) {
            ESP_LOGD(TAG, "聊天屏已重建, 清空气泡记录");
        }
        s_ctx_hooked = ui_contextpanel;
        s_bubble_cnt = 0;
        s_live_label = NULL;
        s_live_res   = NULL;
        s_end_ms     = 0;
        s_asr_active = false;

        /* 删掉 SquareLine 预置的占位气泡("你好"/"hello"), 否则永远和真实消息混在一起。
         * ★ lv_obj_is_valid 只做指针比较、不解引用入参 (lv_obj.c:450) → 悬空也安全。
         * ★ 删完立刻置 NULL, 免得 SquareLine 的 destroy 或别处再引用。 */
        if (ui_merollpanel != NULL && lv_obj_is_valid(ui_merollpanel)) {
            lv_obj_del(ui_merollpanel);
        }
        ui_merollpanel = NULL;
        if (ui_resrollpane != NULL && lv_obj_is_valid(ui_resrollpane)) {
            lv_obj_del(ui_resrollpane);
        }
        ui_resrollpane = NULL;
    }

    /* ③ 兜底: 第一个气泡已不合法 → 整体作废 */
    if (s_bubble_cnt > 0 && !lv_obj_is_valid(s_bubbles[0])) {
        ESP_LOGW(TAG, "气泡指针已失效, 清空气泡记录");
        s_bubble_cnt = 0;
        s_live_label = NULL;
        s_live_res   = NULL;
        s_end_ms     = 0;
    }
    return true;
}

/* 收尾当前气泡: 空的直接删掉(按了没说 / 服务器没回字), 非空的只释放指针
 * (它已经是一条普通的历史消息了)。 */
static void chat_view_close_bubble(void)
{
    if (s_live_label == NULL) {
        return;
    }

    const char *t = lv_label_get_text(s_live_label);
    if (t == NULL || t[0] == '\0') {
        lv_obj_t *roll = lv_obj_get_parent(lv_obj_get_parent(s_live_label));
        if (s_bubble_cnt > 0 && s_bubbles[s_bubble_cnt - 1] == roll) {
            s_bubble_cnt--;
            s_bubbles[s_bubble_cnt] = NULL;
        }
        lv_obj_del(roll);       /* 连带删 panel 和 label */
    }
    s_live_label = NULL;
}

/* 收到一条 ASR 文本 → 上屏 (由 drain_queues 在 lvgl 任务里调用) */
static void chat_view_show_asr(const char *text, bool is_final)
{
    if (text == NULL) {
        return;
    }
    if (!chat_view_sync_screen()) {
        /* 不在聊天屏: 本轮结果只留在控制台。
         * ⚠️ 已知限制: 不会"等你回聊天屏再补上"。若成为困扰, 可加 PSRAM 环形缓冲重放。 */
        ESP_LOGW(TAG, "[上屏] 当前不在聊天屏, 丢弃显示: %s", text);
        return;
    }

    /* ★★ 三种情况:
     *   ① s_live_label != NULL → 正常: 更新当前气泡 (asr 和 final 走同一条路, 全量替换)
     *   ② 为空 + 会话还在进行   → 中途才切到聊天屏, 补建一条
     *   ③ 为空 + 会话已结束     → 【绝不新建】← 这是"一次按键冒两个气泡"的根源
     *
     *   ⚠️ 为什么 ③ 必须堵死 —— 服务器的 final 有【两个时机】, 都不是"本轮结束":
     *      a) **讯飞段边界**: 你【还按着按钮】的时候就回。
     *         实测证据: `[流式] final=1 ...` 在 t=18315, 而松手在 t=19465 —— 早 1.15 秒!
     *      b) **松手之后**: 本地 ENDED 是立刻投的, 而 final 是网络回包, 实测再晚 ~400ms。
     *
     *   早期写法在 final 处就把 s_live_label 置 NULL("这句说完了"), 于是:
     *        段边界的 final → 结掉气泡 → 下一段的 asr 发现没气泡 → 【又建一个】✗
     *        屏幕上就是"按下的那个" + "final 后又建的那个" = 两个气泡。
     *   现在 final 只更新文字、绝不释放指针; 气泡的生命周期交给
     *   STARTED(建) / ENDED+宽限期(收) / 下一次 STARTED(先收再建)。 */
    if (s_live_label != NULL) {
        chat_label_set(s_live_label, text);
        lv_obj_update_layout(ui_contextpanel);
        /* ★ 用 ANIM_OFF: 流式期每 ~137ms 一条, 开动画会一直重启滚动看着抖 */
        lv_obj_scroll_to_view(lv_obj_get_parent(lv_obj_get_parent(s_live_label)), LV_ANIM_OFF);
    } else if (s_asr_active) {
        s_live_label = chat_push(text, true, "会话进行中补建(中途才切到聊天屏)");
    } else {
        ESP_LOGW(TAG, "[上屏] 本轮已收尾, 不再新建气泡: %s", text);
        return;
    }

    if (is_final) {
        ESP_LOGI(TAG, "[上屏] 本句定格: %s  (PSRAM 空闲 %u KB)",
                 text, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
}

/* 会话开始 (按下录音键) → 收尾上一条 + 放一个【空气泡】。
 * ★ 不等有文字再建: "按下"到"服务器回第一个字"要经过 等连接→切服务→svc_ok→start
 *   →开口→识别, 可能一两秒; 这段时间屏幕毫无反应, 用户会以为坏了。
 *   空串让 label 变 0 尺寸, 气泡只剩 padding —— 视觉上就是个刚冒出来的小绿块。 */
static void chat_view_begin_asr(void)
{
    if (!chat_view_sync_screen()) {
        ESP_LOGW(TAG, "[上屏] 会话开始, 但当前不在聊天屏");
        return;
    }

    /* ★ 上一轮还在等 final、还没问 openclaw, 用户就按下了 →
     *   先把那一轮的问题【冲出去】, 不能被下面的重置吞掉。
     *   (s_last_asr / s_asked 就在下面被重置, 所以必须在这之前用) */
    if (s_ask_armed && !s_asked && s_last_asr[0] != '\0') {
        s_asked = true;
        ESP_LOGW(TAG, "[LLM] 新一轮开始, 先把上一轮还没发的兜底转发发出去");
        UiBridge::get().llm_ask(s_last_asr);
    }
    s_final_seen = false;               /* 新一轮, 重新开始等 final */
    s_asked      = false;
    s_last_asr[0] = '\0';
    s_ask_armed  = false;

    chat_view_close_bubble();   /* ★ 上一轮若还挂着(宽限期内又按了), 先结掉再建新的 */
    s_end_ms     = 0;
    s_asr_active = true;
    s_live_label = chat_push("", true, "按下键 STARTED");
}

/* 收到一段 LLM 回复 → 写进"对方"的气泡 (灰底 / 靠左)。
 * ★ 和 ASR 那边的区别: 一次 chat 只产生一条 reply, 没有"多段"问题,
 *   所以 is_final 时直接释放指针即可 (下一条回复会新建气泡)。 */
static void chat_view_show_llm(const char *text, bool is_final)
{
    if (text == NULL) {
        return;
    }
    if (!chat_view_sync_screen()) {
        ESP_LOGW(TAG, "[上屏] 当前不在聊天屏, LLM 回复不显示: %s", text);
        return;
    }

    if (s_live_res != NULL) {
        chat_label_set(s_live_res, text);           /* 复用同一条, 只改文字 */
        lv_obj_update_layout(ui_contextpanel);
        lv_obj_scroll_to_view(lv_obj_get_parent(lv_obj_get_parent(s_live_res)), LV_ANIM_OFF);
    } else {
        s_live_res = chat_push(text, false, "LLM 回复");
    }

    if (is_final) {
        ESP_LOGI(TAG, "[上屏] LLM 回复完成");
        s_live_res = NULL;                          /* 下一条回复新建气泡 */
    }
}

/* 会话结束 (松开 / 中止) → **只记时刻, 不立刻收尾**。
 * ★ 松手后 voice_task 立刻投 ENDED, 而服务器随后(~400ms)才发 final ——
 *   那条 final 是最终修正版, 必须还能落进同一条气泡。
 *   真正的收尾交给 chat_view_tick (宽限期到期) 或下一次 chat_view_begin_asr。
 * ★ 顺带起"等 final 问 openclaw"的兜底表 (见 ask_tick)。 */
static void chat_view_end_asr(void)
{
    s_asr_active = false;
    if (s_live_label != NULL) {
        s_end_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }

    /* 本轮有识别到字才值得去问 openclaw (按了没说就别打扰人家) */
    if (s_last_asr[0] != '\0') {
        s_ask_armed    = true;
        s_ask_deadline = (uint32_t)(esp_timer_get_time() / 1000) + ASK_FINAL_WAIT_MS;
    }
}

/* 每轮调一次 (drain_queues 里): 宽限期过了就真收尾。
 * 这样"按了没说"的空气泡不会一直挂着, 而 final 也来得及落进来。 */
static void chat_view_tick(void)
{
    if (s_end_ms == 0 || s_live_label == NULL) {
        return;
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if ((now - s_end_ms) < CHAT_END_GRACE_MS) {
        return;                 /* 还在等 final */
    }
    chat_view_close_bubble();
    s_end_ms = 0;
}

/* 每轮调一次 (drain_queues 里): "问 openclaw"的兜底。
 *
 * ★★ 为什么必须有它: 转发的首选触发点是 final, 而 final 要等网关先处理完 end 才发。
 *    网关那边一旦有问题(实测就是), final 永远不来 → 永远不问 openclaw → 灰泡不出现,
 *    而现象看起来像"openclaw/气泡功能坏了", 完全指不到真正的原因。
 *    有了它: final 来了就用 final(最准), 2.5 秒还不来就用最后一条 asr 顶上。
 *    两条路靠 s_final_seen 互斥, 保证一轮只问一次。 */
static void ask_tick(void)
{
    if (!s_ask_armed) {
        return;
    }
    if (s_final_seen || s_asked) {
        s_ask_armed = false;            /* final 已经转发过了, 不用兜底 */
        return;
    }
    if ((uint32_t)(esp_timer_get_time() / 1000) < s_ask_deadline) {
        return;                         /* 还没到点, 再等等 final */
    }

    s_ask_armed = false;
    s_asked     = true;
    ESP_LOGW(TAG, "[LLM] 等 final 超时 (%dms), 改用最后一条 asr 兜底转发: %.30s",
             (int)ASK_FINAL_WAIT_MS, s_last_asr);
    UiBridge::get().llm_ask(s_last_asr);
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
    chat_view_sync_screen();

    /* ★ 宽限期检查: 松手后等 final 落进气泡, 超时就真收尾
     *   (这样"按了没说"的空气泡不会一直挂着)。见 CHAT_END_GRACE_MS 的说明。 */
    chat_view_tick();

    /* ★ "问 openclaw"的兜底: final 迟迟不来时, 用最后一条 asr 顶上。
     *   必须排在下面两个 while 之前 —— 因为 STARTED 会重置 s_final_seen / s_last_asr。 */
    ask_tick();

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
                chat_view_begin_asr();      /* 按下 → 收尾上一条 + 放一个空气泡 */
            } else {
                /* ENDED / ABORTED → 只记时刻, 不立刻收尾:
                 * 服务器的 final 是网络回包(~400ms 后才到), 得留个落点,
                 * 否则它到了没处放 → 又新建一个气泡。真正收尾在 chat_view_tick。 */
                chat_view_end_asr();
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
            /* ★ 记下最后一条 —— 兜底转发 (ask_tick) 用的就是它。
             *   在这里更新而不是在 WS 回调里, 是为了和 ask_tick 同处 LVGL 任务, 无竞争。 */
            strlcpy(s_last_asr, s.text, sizeof(s_last_asr));
            chat_view_show_asr(s.text, s.is_final);
            break;

        case STREAM_ABROAD:
            /* 网关的 {"type":"abroad"} 带出来的识别内容 —— 会话已结束, 这是"最后认到的"。
             * ★ 内容和 STREAM_ASR 一样是【我的话】→ 走同一条显示路径 (绿底/靠右)。
             * ★ 也更新 s_last_asr: 它就是最后一条识别结果, 兜底转发该用它。
             * ★ 不碰气泡生命周期 —— 收尾由 resp_q 的 ENDED 驱动 (只有 abroad text=null
             *   才投 VOICE_STOP, 带文本时用户可能还在接着说)。 */
            ESP_LOGW(TAG, "[流式] ABROAD text=\"%s\"", s.text);
            strlcpy(s_last_asr, s.text, sizeof(s_last_asr));
            chat_view_show_asr(s.text, true);
            break;

        case STREAM_LLM:                        /* 对方回复: 灰底 / 靠左 */
            chat_view_show_llm(s.text, s.is_final);
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
