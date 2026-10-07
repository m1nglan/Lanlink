#include "business/ui_bridge.hpp"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"   /* heap_caps_malloc: 明确要求 PSRAM */
#include "cJSON.h"           /* on_ws_error 要解析 {"type":"error","code":N} */

#include "drivers/ws.hpp"

#include "lvgl.h"
#include "ui.h"              /* ui_contextpanel / ui_font_ch14 (INCLUDE_DIRS 里有 "lvgl") */

static const char *TAG = "ui_bridge";

/*! UI 消费周期。partial 再高频, 也只在"队列非空(有新内容)"时才刷。 */
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

    /* asr 绑到 WS 单例 + 注册结果回调。
     * attach 只做"往 WS 的 handler 表登记", 跟"有没有建连"无关 → 这里做一次就够。
     * ★ 重连**不需要**重新 attach: WS::deinit() 不清 handler 表,
     *   且 set_handler() 按 type 自带去重(重复注册不会撑满表)。 */
    m_asr.attach(WS::get());
    m_asr.set_result_callback(&UiBridge::on_asr_result, this);

    /* ★ 服务器的 svc_ok → 投 VOICE_SVC_ACKED, 唤醒在等"切服务完成"的 voice_session。
     *  驱动(WS)不认识业务, 只回调一个函数指针 —— 和上面 set_result_callback 一个套路。 */
    WS::get().set_svc_ok_callback(&UiBridge::on_svc_ok, this);

    /* ★ 网关错误也要接住 —— 否则"音频超过 55 秒"这类错误只会打条日志,
     *   voice_task 还在白发音频。见 on_ws_error 的说明。 */
    WS::get().set_handler("error", &UiBridge::on_ws_error, this);

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
    /* ★ 先把队列里的旧命令清掉, 再投 START —— 建一条不变式:
     *     "START 之后 voice_q 里只会有本轮的命令"。
     *   ⚠️ 不这么做的话: 网关报错(code=1/3)时 on_ws_error 投的 VOICE_STOP
     *      若恰好在用户松手那一刻到达, 会残留到下一轮 → voice_q_drain 一进来
     *      就把它当成"用户松手" → 新一轮【刚按就被取消】, 且日志看起来莫名其妙。
     *   timeout=0: 只清已有的, 绝不阻塞 (本函数可能从 esp_timer 任务调用)。 */
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
 * 下面三个投递接口 (post_resp / post_resp_status / 调用方直接用) 最后都走这里,
 * 所以"非阻塞发送"和"满了告警"只写一份。
 * 传 const 引用 → 只有 xQueueSend 内部那一次拷贝, 不额外吃栈。 */
void UiBridge::post_resp_msg(const resp_msg_t &m)
{
    if (m_resp_q == nullptr) {
        return;
    }
    /* ★ timeout=0 是硬要求: 本函数会在 WS 回调里被调用,
     *   那时正持着 esp_websocket_client 的 client->lock。
     *   若这里阻塞, 整个 WS 收发(含 voice_task 发音频)都会被拖死。 */
    if (xQueueSend(m_resp_q, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "resp_q 满, 丢弃 kind=%d", (int)m.kind);
    }
}

/* 文本类: 标签 + 字符串 (通用短提示)。
 * ★ 只有 64 字节 (≈21 汉字) —— 识别结果/LLM 回复请走 post_stream。
 * ★ 当前没有调用者 (RESP_STATUS 是预留); 留着是因为它让"带数值的短提示"
 *   有地方放, 而 64 × 16 = 1KB 的代价可以接受。 */
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
    /* ★ 顺带维护"本轮是否在录音" —— 只给 on_ws_error 用来决定要不要投 VOICE_STOP。
     *  会话状态只有 STARTED / ENDED / ABORTED 三种, STARTED 之外都算结束。 */
    if (kind == RESP_ASR_STATUS) {
        m_asr_active = (status == STARTED);
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
        /* ★ text 是【完整文本】(V2), 所以这里是整体替换 —— 没有累积。
         *   BUS_TEXT_LEN = 2048 → 一条 680 汉字, 足够一句 / 一段回复。 */
        strlcpy(m.text, text, sizeof(m.text));
    }
    /* xQueueOverwrite: 深度 1 覆盖式 → 永不阻塞、永不失败, 只留最新那句 */
    xQueueOverwrite(m_stream_q, &m);
}

/* ======================= 接收 (LVGL 侧) ======================= */

/* asr 结果回调: 跑在 websocket_task, 且持着 client->lock → 只做一次非阻塞投递。
 * ★ V2: text 已经是【完整句】(网关发的就是全量), 本函数不做任何累积。 */
void UiBridge::on_asr_result(const char *text, bool is_final, void *ctx)
{
    UiBridge *self = static_cast<UiBridge *>(ctx);
    if (self == nullptr || text == nullptr) {
        return;
    }
    /* 全走 stream_q: 覆盖式, 只留最新那句; is_final 标记"本轮完成"。
     * (V1 是 final 走 resp_q / partial 走 stream_q 两条路 —— V2 合并成一条,
     *  消费者就不需要"该替换还是该追加"的判断了。) */
    self->post_stream(STREAM_ASR, text, is_final);
}

/* svc_ok 回调: 同 on_asr_result, 跑在 websocket_task 且持着 client->lock
 * → 只做一次非阻塞投递 (timeout=0), 绝不阻塞。 */
void UiBridge::on_svc_ok(const char *service, void *ctx)
{
    (void)service;   /* 当前只有一个服务(text), 不需要区分; 以后多服务时可用 */
    UiBridge *self = static_cast<UiBridge *>(ctx);
    if (self == nullptr) {
        return;
    }
    self->push_voice_cmd(VOICE_SVC_ACKED, 0);
}

/* 网关错误回调: 跑在 websocket_task 且持着 client->lock → 只做非阻塞投递。
 *
 * 目的: 网关判定"这一轮不要了"时, 让 voice_task 立刻收尾, 而不是继续白发音频。
 * 用 VOICE_STOP 走现成通道 —— 采音循环的 STOP 检查会接住它, 走正常收尾路径
 * (mic.stop + asr.end), 所以服务器那边的 final 还能回来。
 *
 * ⚠️ 必须用 m_asr_active 守卫: 不在录音时投 STOP 的话, 那个 STOP 会残留在
 *    voice_q 里, 被下一次会话开头的 voice_q_drain 当成"用户松手"而取消。
 *    (voice_start() 也会先清队列, 两道防线。) */
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

/* ================================================================
 * 聊天视图: 气泡 (v3, 只做"我的消息")
 *
 * 结构 (与 SquareLine 导出的一致, 属性必须照抄否则外观会跑偏):
 *   ui_contextpanel (flex column, 可滚动)
 *   └── roll  (wrapper, 310 宽 × SIZE_CONTENT, 透明)      ← flex 孩子
 *       └── panel (气泡, SIZE_CONTENT, align=TOP_RIGHT)   ← 父无 layout → align 生效
 *           └── label (文字, SIZE_CONTENT, max_width 280)
 *
 * ★ 为什么 wrapper 里还要用 align:
 *   flex 的交叉轴对齐是【整容器一个值】, 所有子对象一样 —— 做不到"我的靠右、
 *   对方的靠左"。所以每条消息套一层 wrapper: wrapper 是 flex 孩子(被 flex 摆),
 *   气泡是 wrapper 的孩子而 wrapper 没有 layout → 气泡的 align 生效
 *   (lv_obj_pos.c:779 `lv_obj_refr_pos` 见到父有 layout 就直接 return)。
 * ================================================================ */

#define CHAT_MAX_BUBBLES  (10)     /*!< 最多留几条消息 (超限删最老的) */

/* 存 wrapper 指针 —— 删 wrapper 会连带删掉里面的气泡和标签 */
static lv_obj_t *s_bubbles[CHAT_MAX_BUBBLES];
static int       s_bubble_cnt = 0;

/* "正在说"的那条 (流式期间反复改它的文字); 定格后置 NULL, 下一条新建 */
static lv_obj_t *s_live_label = NULL;

/* 上次记录的那些指针属于哪个 contextpanel —— 用来识别"聊天屏被销毁重建了" */
static lv_obj_t *s_ctx_hooked = NULL;

/* ★ 气泡文字要不要放 PSRAM?
 *   1 = lv_label_set_text_static + 自己 heap_caps_malloc(MALLOC_CAP_SPIRAM)
 *   0 = lv_label_set_text, 让 LVGL 拷到它自己的堆 (内部 SRAM) —— 和 SquareLine 一致
 *
 * ⚠️⚠️ 实测: 设成 1 时【气泡会变长但文字一个都不画】(占位空白)。
 *     已排除字体 (换成 "你好" 也不显示; 字体是常用 7000 字, 且 .fallback=NULL 只是不兜底)。
 *     set_text_internal 和 set_text_static 结尾都是同一句 lv_label_mark_need_refr_text,
 *     渲染路径一样 → 问题在别处, 尚未定位。
 *     所以先设 0 保证能用; 查清 static 路径再打开。
 *
 * 内存账 —— 为什么现在设 0 也没关系:
 *   一条识别结果通常 20~40 个汉字 = 60~120 字节 (BUS_TEXT_LEN=2048 是极端上限);
 *   10 条 ≈ 1~2 KB 内部 SRAM, 可以忽略。
 *   真正需要 PSRAM 的是【LLM 长回复】(每条可能上千字节 → 10 条 20KB), 到那时再解决。 */
#define CHAT_TEXT_IN_PSRAM  (0)

#if CHAT_TEXT_IN_PSRAM
/* 标签被删时, 释放我们自己分配的 PSRAM 文字副本。
 * ★ 必须自己管: lv_label_set_text_static 之后 LVGL 永不释放文字
 *   (lv_label.c:774 `if(!label->static_txt) lv_free(label->text);`) → 归我们。 */
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

/* 把文字设进标签。
 *
 * 路径 A (=0, 当前): lv_label_set_text —— LVGL 自己拷贝到它的堆 (内部 SRAM)。
 *   和 SquareLine 导出的写法完全一致, 已验证可用。
 *
 * 路径 B (=1): 自己 heap_caps_malloc(MALLOC_CAP_SPIRAM) + set_text_static。
 *   ⚠️ 当前不可用 (文字不画), 原因未定位 —— 见上面 CHAT_TEXT_IN_PSRAM 的说明。 */
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

/* 新建一个"我的消息"气泡 (绿底、靠右)。返回文字标签指针。 */
static lv_obj_t *chat_add_me_bubble(const char *text)
{
    /* ---- 1. wrapper (flex 孩子) ---- */
    lv_obj_t *roll = lv_obj_create(ui_contextpanel);
    lv_obj_set_width(roll, 310);
    lv_obj_set_height(roll, LV_SIZE_CONTENT);
    lv_obj_remove_flag(roll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(roll, 0, 0);
    lv_obj_set_style_bg_opa(roll, 0, 0);         /* 透明 */
    lv_obj_set_style_border_opa(roll, 0, 0);
    lv_obj_set_style_pad_all(roll, 0, 0);

    /* ---- 2. 气泡 (align 在这里生效) ---- */
    lv_obj_t *panel = lv_obj_create(roll);
    lv_obj_set_size(panel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_align(panel, LV_ALIGN_TOP_RIGHT);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x35D28D), 0);
    lv_obj_set_style_bg_opa(panel, 255, 0);
    lv_obj_set_style_border_opa(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, 8, 0);
    lv_obj_set_style_pad_right(panel, 8, 0);
    lv_obj_set_style_pad_top(panel, 6, 0);
    lv_obj_set_style_pad_bottom(panel, 6, 0);
    /* ★ 这里【故意不设 radius】—— 和 SquareLine 导出的 ui_mepanel 保持一致:
     *   样例也没设, 所以走主题默认 (lv_theme_default.c:27 的 RADIUS_DEFAULT,
     *   随 DPI 缩放)。要是自己写死一个数, 就和 SquareLine 里看到的不一样了。
     *   想要圆角的话在 SquareLine 里给 ui_mepanel 加上 radius, 我再照抄过来。 */

    /* ---- 3. 文字 (默认已是 LV_LABEL_LONG_WRAP, 见 lv_label.c:748) ---- */
    lv_obj_t *label = lv_label_create(panel);
    lv_obj_set_size(label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(label, 280, 0);
    lv_obj_set_style_text_font(label, &ui_font_ch14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x000000), 0);
#if CHAT_TEXT_IN_PSRAM
    lv_obj_add_event_cb(label, chat_label_deleted_cb, LV_EVENT_DELETE, NULL);
#endif
    chat_label_set(label, text);

    return label;
}

/* 新建并登记一条消息, 超限删最老的 */
static lv_obj_t *chat_push_me(const char *text)
{
    if (s_bubble_cnt >= CHAT_MAX_BUBBLES) {
        /* 删 wrapper → 连带删气泡和标签; 标签的 LV_EVENT_DELETE 会释放 PSRAM 文字 */
        lv_obj_del(s_bubbles[0]);
        memmove(s_bubbles, s_bubbles + 1,
                sizeof(s_bubbles[0]) * (CHAT_MAX_BUBBLES - 1));
        s_bubble_cnt--;
    }

    lv_obj_t *label = chat_add_me_bubble(text);
    lv_obj_t *roll  = lv_obj_get_parent(lv_obj_get_parent(label));   /* label→panel→roll */
    s_bubbles[s_bubble_cnt++] = roll;

    /* 新气泡的坐标要等布局算完才有 → 先更新一次布局再滚 (本函数在 lv_timer 回调里跑,
     * 此时 layout 锁不在手上; 万一在, lv_obj_update_layout 会直接 early return,
     * 见 lv_obj_pos.c:383 —— 不会死锁)。 */
    lv_obj_update_layout(ui_contextpanel);
    lv_obj_scroll_to_view(roll, LV_ANIM_ON);

    return label;
}

/* ================================================================
 * 每次上屏前同步一次屏幕状态。返回 false = 现在不能上屏。
 *
 * ★★ 为什么需要这个:
 *   SquareLine 的聊天屏是【按需创建 + 离开即销毁】的
 *   (ui_chat.c:40 `lv_obj_add_event_cb(..., LV_EVENT_SCREEN_UNLOADED, ...ui_chat_screen_destroy)`)。
 *   所以 ui_contextpanel 会换新对象, 而我们记在 s_bubbles[] / s_live_label 里的
 *   指针会全部变成悬空指针 —— 下一次 lv_obj_del(s_bubbles[0]) 就是 use-after-free。
 *
 * 这里做三件事:
 *   ① 不在聊天屏 (ui_contextpanel == NULL) → 什么都不做
 *      ⚠️ 绝不能拿 NULL 去 lv_obj_create —— 那会创建一个【新的屏幕】而不是子对象。
 *   ② contextpanel 换对象了 → 清空记录 + 清掉 SquareLine 预置的占位气泡
 *   ③ 兜底: 指针万一失效 (别的路径销毁了屏) → 也清空
 * ================================================================ */
static bool chat_view_sync_screen(void)
{
    if (ui_contextpanel == NULL) {
        /* 还没进过聊天屏, 或已经离开了 → 忘掉一切, 绝不能拿去创建对象 */
        s_bubble_cnt  = 0;
        s_live_label  = NULL;
        s_ctx_hooked  = NULL;
        return false;
    }

    if (s_ctx_hooked != ui_contextpanel) {
        /* 屏幕换了 (第一次进, 或者离开过又进来) → 旧指针全部作废 */
        if (s_ctx_hooked != NULL) {
            ESP_LOGD(TAG, "聊天屏已重建, 清空气泡记录");
        }
        s_ctx_hooked = ui_contextpanel;
        s_bubble_cnt = 0;
        s_live_label = NULL;

        /* 删掉 SquareLine 预置的占位气泡 ("你好" / "hello")。
         * 不然它们会永远挂在 contextpanel 里, 和真实消息混在一起。
         * ★ lv_obj_is_valid 只做指针比较 (lv_obj.c:450, 遍历对象树比对),
         *   不解引用入参 → 对悬空指针也安全。
         * ★ 删完立刻把全局置 NULL, 免得 SquareLine 的 destroy 或别处再引用它。 */
        if (ui_merollpanel != NULL && lv_obj_is_valid(ui_merollpanel)) {
            lv_obj_del(ui_merollpanel);
        }
        ui_merollpanel = NULL;
        if (ui_resrollpane != NULL && lv_obj_is_valid(ui_resrollpane)) {
            lv_obj_del(ui_resrollpane);
        }
        ui_resrollpane = NULL;
    }

    /* ③ 兜底: 记录里的第一个气泡已经不合法 → 整体作废 */
    if (s_bubble_cnt > 0 && !lv_obj_is_valid(s_bubbles[0])) {
        ESP_LOGW(TAG, "气泡指针已失效, 清空气泡记录");
        s_bubble_cnt = 0;
        s_live_label = NULL;
    }
    return true;
}

/* 收到一条 ASR 文本 → 上屏 (drain_queues 里调用, 跑在 lvgl 任务) */
static void chat_view_show_asr(const char *text, bool is_final)
{
    if (text == NULL) {
        return;
    }
    if (!chat_view_sync_screen()) {
        /* 不在聊天屏: 本轮识别结果只留在控制台。
         * ⚠️ 已知限制: 现在不会"等你回到聊天屏再补上"。若这变成实际困扰,
         *    可以在这里把文字存进一个 PSRAM 环形缓冲, 回到聊天屏时重放。 */
        ESP_LOGW(TAG, "[上屏] 当前不在聊天屏, 丢弃显示: %s", text);
        return;
    }

    if (!is_final) {
        /* 流式: 复用同一条气泡, 只改文字 (V2 下 text 就是完整句, 直接覆盖) */
        if (s_live_label == NULL) {
            s_live_label = chat_push_me(text);
        } else {
            chat_label_set(s_live_label, text);
            lv_obj_update_layout(ui_contextpanel);
            /* ★ 流式期用 ANIM_OFF: 每 ~137ms 就来一条, 开动画会一直重启滚动动画,
             *   看着抖。气泡每轮只长一点, 直接跳反而更稳。
             *   (定格/新建气泡时用 ANIM_ON, 见 chat_push_me) */
            lv_obj_scroll_to_view(lv_obj_get_parent(lv_obj_get_parent(s_live_label)), LV_ANIM_OFF);
        }
    } else {
        /* 定格: 确保最终文字显示出来, 然后释放 live 指针 (下一条消息新建气泡) */
        if (s_live_label == NULL) {
            s_live_label = chat_push_me(text);
        } else {
            chat_label_set(s_live_label, text);
        }
        ESP_LOGI(TAG, "[上屏] 定格: %s  (PSRAM 空闲 %u KB)",
                 text, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        s_live_label = NULL;
    }
}

void UiBridge::drain_queues(void)   //< LVGL 侧把两个队列取空 (见 PROTOCOL.md)
{
    /* ★ stream_msg_t 现在 2056 字节 (BUS_TEXT_LEN=2048), 而本函数跑在 lvgl 任务
     *   (栈 6K, 且 lv_timer_handler 内部还会嵌套调用)里 → 必须放 static,
     *   绝不能放栈上。本函数只被 ui_timer_cb 调用(单线程上下文), 所以 static 安全。 */
    static stream_msg_t s;
    static resp_msg_t   r;

    /* ★★ 每轮都同步一次屏幕状态 —— 不是只在上屏时。
     *   原因: 同步里会删掉 SquareLine 预置的占位气泡 ("你好" / "hello")。
     *   如果只在 chat_view_show_asr 里同步, 那就要【等用户说第一句话】才删 ——
     *   进聊天屏后到开口之前, 屏幕上一直挂着两个假气泡。
     *   放到这里 (~137ms 一轮) 就等于"一进聊天屏就清干净"。
     *   本函数幂等, 重复调用只花一次 lv_obj_is_valid 遍历, 可忽略。 */
    chat_view_sync_screen();

    /* ---- 流式通道: 覆盖队列, "非空"就等于"有变化", 拿到才刷 ----
     * ★★ V2 的核心收益: s.text 永远是【完整文本】, 所以这里【零累积状态】——
     *    收到什么就显示什么, 不需要判断"该替换还是该追加"。
     *    (V1 要维护 s_result / 判断 partial vs final / 处理 revise 覆盖,
     *     实测出过"revise 的片段把累积好的整句冲掉"这种 bug。) */
    while (xQueueReceive(m_stream_q, &s, 0) == pdTRUE) {
        /* ★ 本阶段只做【我的消息】(语音识别结果) —— LLM 回复留到后面 */
        if (s.kind != STREAM_ASR) {
            ESP_LOGI(TAG, "[流式] kind=%d (本阶段只上屏 ASR) text=\"%s\"", (int)s.kind, s.text);
            continue;
        }
        chat_view_show_asr(s.text, s.is_final);
    }

    /* ---- 控制通道: 可靠队列, 逐条处理 (只走状态和短提示) ----
     * 本阶段只 log; 阶段 3 换成 switch (r.kind) 分派到具体控件。
     * ★ r.kind 决定读 u 的哪一项 —— 状态类是 u.sta, 其余是 u.text,
     *   不能一律按 text 打 (状态类的 u.text 是枚举字节, 会打成乱码)。 */
    while (xQueueReceive(m_resp_q, &r, 0) == pdTRUE) {
        if (r.kind == RESP_ASR_STATUS || r.kind == RESP_WS_STATUS) {
            ESP_LOGI(TAG, "[结果] kind=%d t=%ums status=%d",
                     (int)r.kind, (unsigned)r.t_ms, (int)r.u.sta);
        } else {
            ESP_LOGI(TAG, "[结果] kind=%d t=%ums text=\"%s\"",
                     (int)r.kind, (unsigned)r.t_ms, r.u.text);
        }
    }
}

/* lv_timer 回调。★ 本函数由 lv_timer_handler() 调用, 此时 LVGL 锁已被持有,
 * 所以只能调用**不带锁**的内部函数, 绝不能碰 lvgl_port_lock()。 */
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
    ESP_LOGI(TAG, "UI 消费 timer 已注册 (每 %dms 取一次队列, 本阶段只 log)",
             UI_TIMER_PERIOD_MS);
    return ESP_OK;
}
