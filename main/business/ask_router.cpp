#include "business/ask_router.hpp"

#include <string.h>
#include <stdio.h>           /* snprintf: 拼 "[Lanlink] " 前缀 */

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "ask_router";

/*! 语音识别结果转发给哪个服务。
 *  "openclaw" = 明岚   |   "llm" = DeepSeek
 *  ★ 两者协议完全相同 (chat / partial / reply), 换的只是服务名 ——
 *    想切回来只改这一行。 */
#define ASK_SERVICE  "openclaw"

/*! 提问前缀 —— 让对方一眼看出这话是从 Lanlink 设备来的。
 *  实际发出的是 {"type":"chat","content":"[Lanlink] <识别文字>"}。
 *  ★ 尾巴上带个空格, 免得和第一个字粘在一起。 */
#define ASK_PREFIX   "[Lanlink] "

/*! 问题已发出、等回复期间占位用的灰泡文字。
 *  ★ 明岚调工具时实测要 3~25 秒 (多在 20 秒上下) 才吐第一个字, 这段空窗期
 *    屏幕上什么都没有, 用户会以为死机了。
 *  ★ 怎么"消失"的: 它就是一条普通的 STREAM_LLM 文本, 第一条 partial/reply
 *    到达时 chat_view::show_llm 会把当前灰泡的文字整个覆盖掉 —— 自动替换,
 *    不需要任何删除逻辑。 */
#define ASK_WAIT_TEXT  "(思考中)"

/*! 松手后最多等 final 多久 (ms), 超了就用最后一条 asr 兜底去问。
 *
 * ★★ 为什么需要兜底: 触发转发靠的是 final, 而 final 是网关【收到 end 之后】才发的。
 *    那条路一旦出问题, final 永远不来 → 就永远不问, 灰泡功能整个失效,
 *    而且从现象上看像"openclaw 坏了", 极难定位。
 *    有了兜底: final 来了用 final(最准), 不来就用最后一条 asr(通常只差个句号)。
 * ★ 取 2.5 秒: final 实测在 end 之后约 1470ms 到, 给足余量又不至于让用户干等。
 *   与 chat_view 的 CHAT_END_GRACE_MS 同值 —— 描述的是同一件事。 */
#define ASK_FINAL_WAIT_MS  (2500)

namespace ask_router {

/* ------------------ 注入的回调 ------------------ */
static hooks_t s_hooks = {};

/* ------------------ 状态 ------------------ */
/*! 待发的提问 —— send_now() 先切服务, 等 svc_ok 到了再真正发 chat。
 *  ★ 为什么不在切服务的地方等: 那条路跑在 WS 回调 (websocket_task 正持 client->lock),
 *    阻塞会把整个 WS 收发拖死。所以只能"存下来, 等回调里补发"。 */
static char     s_pending_chat[512];   /*!< 一句话 512 绰绰有余 (BUS_TEXT_LEN 是 2048) */
static bool     s_pending = false;

/*! 收到过 final 吗? 用来取消兜底 (final 一到就没必要再等) */
static volatile bool s_final_seen = false;
/*! 本轮【已经问过】了吗 —— 三条触发路径 (final / 超时兜底 / 下一轮冲出去)
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

/* ------------------ 内部工具 ------------------ */

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* 把一句话发给 ASK_SERVICE: 切服务 + 暂存这句话(带 ASK_PREFIX 前缀),
 * 等 svc_ok 回来再发 chat。
 * ★ 不切回 text —— 下一轮录音时 voice_session 开头自己会切回。
 * ★ 用 snprintf 而不是 strlcpy: 要拼前缀, 且 content 过长时它安全截断。 */
static bool send_now(const char *content)
{
    if (content == nullptr || content[0] == '\0') {
        return false;
    }

    snprintf(s_pending_chat, sizeof(s_pending_chat), ASK_PREFIX "%s", content);
    s_pending = true;

    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (s_hooks.switch_service != nullptr) {
        ret = s_hooks.switch_service(ASK_SERVICE);   /* {"type":"svc","service":...} */
    }

    ESP_LOGI(TAG, "转发识别结果到 %s (%.24s%s): %s, 等 svc_ok 后发 chat",
             ASK_SERVICE, content, (content[24] != '\0') ? "..." : "", esp_err_to_name(ret));
    return ret == ESP_OK;
}

/* ------------------ 对外接口 ------------------ */

void init(const hooks_t *hooks)
{
    if (hooks == nullptr) {
        return;
    }
    s_hooks = *hooks;
}

void on_asr(const char *text, bool is_final)
{
    if (text == nullptr) {
        return;
    }
    if (!is_final) {
        return;                 /* 流式中间结果不触发转发 —— 只认最终版 */
    }

    /* ★★ final 到了 → 立刻转发 (这是最准的触发点: 它是整句的最终版)。
     *  ⚠️ 但不能【只】靠它 —— 网关那条路一旦出问题 final 永远不来,
     *     那样就永远不会去问, 灰泡功能整个失效。
     *     所以还会在 ENDED 之后起一个超时兜底 (见 tick / ASK_FINAL_WAIT_MS)。
     *  s_asked 是"本轮只问一次"的闸: 若兜底已经抢先用最后一条 asr 问过了
     *  (final 迟到了 2.5 秒以上), 这里就不再问, 免得冒出两个灰泡。 */
    s_final_seen = true;
    if (!s_asked) {
        s_asked = true;
        send_now(text);
    }
}

void remember_last_asr(const char *text)
{
    if (text == nullptr) {
        return;
    }
    strlcpy(s_last_asr, text, sizeof(s_last_asr));
}

void on_session_end(void)
{
    /* 本轮有识别到字才值得去问 (按了没说就别打扰人家) */
    if (s_last_asr[0] != '\0') {
        s_ask_armed    = true;
        s_ask_deadline = now_ms() + ASK_FINAL_WAIT_MS;
    }
}

void on_session_start(void)
{
    /* ★ 上一轮还在等 final、还没问出去, 用户就按下了 →
     *   先把那一轮的问题【冲出去】, 不能被下面的重置吞掉。
     *   (s_last_asr / s_asked 就在下面被重置, 所以必须在这之前用) */
    if (s_ask_armed && !s_asked && s_last_asr[0] != '\0') {
        s_asked = true;
        ESP_LOGW(TAG, "新一轮开始, 先把上一轮还没发的兜底转发发出去");
        send_now(s_last_asr);
    }
    s_final_seen  = false;      /* 新一轮, 重新开始等 final */
    s_asked       = false;
    s_last_asr[0] = '\0';
    s_ask_armed   = false;
}

void tick(void)
{
    if (!s_ask_armed) {
        return;
    }
    if (s_final_seen || s_asked) {
        s_ask_armed = false;            /* final 已经转发过了, 不用兜底 */
        return;
    }
    if (now_ms() < s_ask_deadline) {
        return;                         /* 还没到点, 再等等 final */
    }

    s_ask_armed = false;
    s_asked     = true;
    ESP_LOGW(TAG, "等 final 超时 (%dms), 改用最后一条 asr 兜底转发: %.30s",
             (int)ASK_FINAL_WAIT_MS, s_last_asr);
    send_now(s_last_asr);
}

bool handle_svc_ok(const char *service)
{
    if (service == nullptr || strcmp(service, ASK_SERVICE) != 0) {
        return false;                   /* 不是本模块等的那个服务 */
    }

    if (s_pending) {
        s_pending = false;
        esp_err_t ret = ESP_ERR_INVALID_STATE;
        if (s_hooks.send_chat != nullptr) {
            ret = s_hooks.send_chat(s_pending_chat);   /* ★ 现在才发 */
        }
        ESP_LOGI(TAG, "%s 已就绪, 发 chat (%.24s%s): %s",
                 ASK_SERVICE, s_pending_chat,
                 (s_pending_chat[24] != '\0') ? "..." : "", esp_err_to_name(ret));

        /* ★ 发成功就立刻放一个"(思考中)"的灰泡占位。
         *   接下来十几二十秒对方都不会吐字, 没这个占位屏幕是空的。
         *   真回复的第一条 partial 到达时会把它整条覆盖掉 —— 见 ASK_WAIT_TEXT 说明。 */
        if (ret == ESP_OK && s_hooks.show_waiting != nullptr) {
            s_hooks.show_waiting(ASK_WAIT_TEXT);
        }
    }
    return true;                        /* ★ 已处理, 调用者别再往下走 */
}

}  // namespace ask_router
