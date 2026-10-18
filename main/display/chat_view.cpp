#include "display/chat_view.hpp"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "lvgl.h"
#include "ui.h"              /* ui_contextPanel / ui_merollpanel / ui_resrollpanel / ui_font_ch14 */

static const char *TAG = "chat_view";

namespace chat_view {

/* ================================================================
 * 实现 —— 全部 static, 链接性不外露 (只有上面头文件那 6 个函数是公开的)。
 * 定义顺序: 被调用的工具在前, 调用者在后 (C 语言先声明后使用)。
 * ================================================================ */

/* ================================================================
 * 聊天视图: 双向气泡
 *
 * [链] 上行(我说的话):
 *   stream_q(STREAM_ASR) → show_asr → push(text, is_me=true, …)
 *                        → add_bubble → label_set
 * [链] 下行(对方回复):
 *   on_asr_result(is_final) → ask_router::on_asr → 切服务 → 等 svc_ok → chat → 网关
 *      → Llm::handle_partial/reply (驱动侧累积成完整回复)
 *      → on_llm_stream → stream_q(STREAM_LLM) → show_llm
 *                      → push(text, is_me=false, …)
 * [链] 会话状态(管气泡生死):
 *   resp_q → begin_asr (STARTED, 按下就出空气泡)
 *          → end_asr  (ENDED/ABORTED, 只记时刻)
 *          → tick     (宽限期到期才真收尾)
 *   ★ 以上全部由 drain_queues 在 lvgl 任务里驱动, 且先过 sync_screen。
 *
 * 控件三层 (属性要与 SquareLine 一致): ui_contextPanel(flex column, 可滚)
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
 *    然后撞上 show_asr 的"③ 已收尾, 不再新建气泡"被丢掉,
 *    屏幕停在最后一条 asr 上(差个句号), 而日志里 final 明明到了, 极难查。
 *    改成 2500 后余量 ≈ 1030ms。
 *
 *  ★ 与 ask_router 的 ASK_FINAL_WAIT_MS 取同一个值: 两者都是"final 该在多久内到"
 *    这一件事, 同值可以避免读代码时来回换算。语义也一致 ——
 *    这段时间内 final 到了就用 final, 没到就用最后一条 asr。 */
#define CHAT_END_GRACE_MS (2500)

/* 存 wrapper 指针 —— 删 wrapper 会连带删掉里面的气泡和标签 */
static lv_obj_t *s_bubbles[CHAT_MAX_BUBBLES];
static int       s_bubble_cnt = 0;

/* "当前这一轮"的气泡标签 (流式期间反复改它的文字)。
 * 由 STARTED 建、由【宽限期到期】或下一次 STARTED 收尾 —— 见 tick/begin_asr。 */
static lv_obj_t *s_live_label = NULL;

/* "对方"(LLM 回复) 当前正在写的那条气泡。比 ASR 那边简单得多:
 * 一次 chat 只有一条 reply, 所以 is_final 时直接释放即可 (没有多段问题)。 */
static lv_obj_t *s_live_res = NULL;

/* ENDED 的时刻 (esp_timer ms); 0 = 没有待收尾的 */
static uint32_t  s_end_ms = 0;

/* 本轮语音会话是否还在进行 (由 STARTED / ENDED 维护)。
 * 用途: 文字到达时若气泡已被收尾【就不要新建】—— 那正是"冒第二个气泡"的来源。 */
static bool      s_asr_active = false;

/* 上次挂钩的是哪个 contextPanel —— 用来做【一次性初始化】(删占位气泡)。
 * ⚠️ 原来它还兼"屏幕重建检测", 但现在的导出**离开聊天屏不销毁** →
 *    ui_contextPanel 指针从头到尾不变 → 这个判断只在开机第一次成立。 */
static lv_obj_t *s_ctx_hooked = NULL;

/* ================================================================
 * 环形缓冲 —— 历史消息的文本备份
 *
 * ⚠️ **当前它基本处于"休眠"状态** —— 说清楚免得误会:
 *
 *   ① 旧导出: 聊天屏**离开即销毁** (LV_EVENT_SCREEN_UNLOADED), 一换屏气泡全没,
 *      靠这里存纯文本, 回来时 ring_replay() 重放。
 *   ② 现导出: 聊天屏**开机就建 + 永不销毁** → 面板一直在 → 气泡也一直在
 *      → 换屏回来消息本来就还在, **根本不需要重放**。
 *
 *   ⇒ 所以现在 ring_replay() 只在开机那一次跑 (缓冲是空的, 无操作), 
 *     ring_append/ring_update 照常记录 —— 相当于【保险】:
 *     万一以后 SquareLine 又把"离开时销毁"勾回来, 这套仍然能工作。
 *
 * ★ 存的是文本 + 谁说的, **不是 LVGL 对象指针** —— 对象可能被销毁, 存指针会悬空。
 * ★ 容量固定 (CHAT_RING_SLOTS), 满了覆盖最老的。当前放 PSRAM (8MB 富余);
 *   以后接 TF 卡之类的外部存储时, 换掉 ring_* 这几个函数的实现即可。
 * ================================================================ */
#define CHAT_RING_SLOTS  (CHAT_MAX_BUBBLES)   /*!< 想存更多就调这里 (重放时只会显示最后 CHAT_MAX_BUBBLES 条) */

typedef struct {
    char *text;      /*!< PSRAM 里的文字副本; NULL = 空槽 */
    bool  is_me;
} ring_slot_t;

static ring_slot_t s_ring[CHAT_RING_SLOTS];
static int s_ring_head = 0;    /*!< 下一个写入位置 */
static int s_ring_cnt  = 0;    /*!< 有效条数 (≤ CHAT_RING_SLOTS) */

/*! 当前活跃气泡对应的槽位 (流式刷新时要去改它); -1 = 没有活跃的。
 *  两个方向各一个, 对应 s_live_label / s_live_res。 */
static int s_live_slot_me  = -1;
static int s_live_slot_res = -1;

/* 气泡文字显式放 PSRAM。
 *
 * ★ 为什么: LVGL 自己的堆在内部 SRAM, 而长回复(LLM 可上千字节)×多条会把内部 SRAM 吃掉。
 *   PSRAM 有 8MB 富余, 放文字正合适 —— 文字是低频访问的, 不影响渲染速度。
 * ★ 做法: heap_caps_malloc(MALLOC_CAP_SPIRAM) + lv_label_set_text_static
 *   (LVGL 只存指针、不拷贝也不释放) → 所有权归我们, 靠 LV_EVENT_DELETE 回收。
 * ⚠️ 曾经的现象是"气泡变长但文字不画", 一度以为这条路坏了 ——
 *   实际根因是 ui_font_ch14 的压缩字体坑 (见 HANDOFF §6-C), 与这条路径无关。 */

/* 标签被删时释放我们自己分配的 PSRAM 文字副本。
 * ★ set_text_static 之后 LVGL 永不释放文字 (lv_label.c:774) → 必须归我们管。 */
static void label_deleted_cb(lv_event_t *e)
{
    lv_obj_t *label = (lv_obj_t *)lv_event_get_target(e);
    char *buf = (char *)lv_obj_get_user_data(label);
    if (buf != nullptr) {
        heap_caps_free(buf);              /* PSRAM 和内部堆都能 free */
        lv_obj_set_user_data(label, NULL);
    }
}

/* 把文字设进标签 —— 复制一份到 PSRAM, 交给标签静态引用。 */
static void label_set(lv_obj_t *label, const char *text)
{
    if (label == NULL || text == NULL) {
        return;
    }

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
        heap_caps_free(old);                /* 流式刷新时走这里, 不会泄漏 */
    }
}

/* ---------- 环形缓冲的读写 (只有这几个函数碰 s_ring) ---------- */

/*! 把一条消息写进缓冲, 返回槽位号 (满则覆盖最老的)。
 *  ★ 文字在 PSRAM 里另存一份 —— 不能用标签里那份, 标签说没就没了。 */
static int ring_append(const char *text, bool is_me)
{
    ring_slot_t *s = &s_ring[s_ring_head];

    size_t n = (text != NULL) ? strlen(text) + 1 : 1;
    char  *buf = (char *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        buf = (char *)heap_caps_malloc(n, MALLOC_CAP_DEFAULT);   /* 退回内部堆 */
        if (buf == NULL) {
            ESP_LOGE(TAG, "环形缓冲分配失败 (%u 字节), 本条不进历史", (unsigned)n);
            return -1;
        }
    }
    if (text != NULL) {
        memcpy(buf, text, n);
    } else {
        buf[0] = '\0';
    }

    if (s->text != NULL) {
        heap_caps_free(s->text);      /* 覆盖最老的那条 */
    }
    s->text  = buf;
    s->is_me = is_me;

    int slot = s_ring_head;
    s_ring_head = (s_ring_head + 1) % CHAT_RING_SLOTS;
    if (s_ring_cnt < CHAT_RING_SLOTS) {
        s_ring_cnt++;
    }
    return slot;
}

/*! 就地更新某个槽位的文字 (流式刷新时用, 不然重放出来的是第一次那个残句)。
 *  ★ 分配失败就保持原样 —— 历史里留个旧版本, 好过丢整条。 */
static void ring_update(int slot, const char *text)
{
    if (slot < 0 || slot >= CHAT_RING_SLOTS || text == NULL) {
        return;
    }
    size_t n   = strlen(text) + 1;
    char  *buf = (char *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        buf = (char *)heap_caps_malloc(n, MALLOC_CAP_DEFAULT);
        if (buf == NULL) {
            return;
        }
    }
    memcpy(buf, text, n);
    if (s_ring[slot].text != NULL) {
        heap_caps_free(s_ring[slot].text);
    }
    s_ring[slot].text = buf;
}

/*! 新建一个气泡。is_me=true → 我的消息(绿底/靠右); false → 对方回复(灰底/靠左)。
 * 返回文字标签指针。
 *
 * ★ 两层 wrapper 是必要的: LVGL 的 flex 交叉轴对齐是【整容器一个值】, 做不到
 *   "我的靠右、对方的靠左"。所以每条消息套一层 wrapper: wrapper 是 flex 孩子
 *   (自己的 x/y/align 无效), 气泡是 wrapper 的孩子而 wrapper 没有 layout
 *   → 气泡的 align 生效 (lv_obj_pos.c:777 见到父有 layout 就直接 return)。
 * ★ 属性照抄 SquareLine 导出的 ui_mepanel / ui_respanel, 别自己发挥(比如加 radius)。 */
static lv_obj_t *add_bubble(const char *text, bool is_me)
{
    /* ---- 1. wrapper (flex 孩子) ---- */
    lv_obj_t *roll = lv_obj_create(ui_contextPanel);
    lv_obj_set_width(roll, is_me ? 311 : 314);   /* 和 ui_merollpanel(311) / ui_resrollpanel(314) 一致 */
    lv_obj_set_height(roll, LV_SIZE_CONTENT);
    lv_obj_remove_flag(roll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(roll, 0, 0);
    lv_obj_set_style_bg_opa(roll, 0, 0);         /* 透明 */
    lv_obj_set_style_border_opa(roll, 0, 0);
    lv_obj_set_style_pad_all(roll, 0, 0);
    if (!is_me) {
        lv_obj_set_style_pad_left(roll, 5, 0);   /* 和 ui_resrollpanel 一致 */
    }

    /* ---- 2. 气泡 (align 在这里生效) ---- */
    lv_obj_t *panel = lv_obj_create(roll);
    lv_obj_set_size(panel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    /* ★ 和模板一致: 我的 RIGHT_MID (靠右居中), 对方的 LEFT_MID (靠左居中)。
     *   wrapper 高度是 SIZE_CONTENT = 气泡自身高度 → 没有纵向余量, 所以 MID 和 TOP 等价;
     *   但保持和 SquareLine 预览同一个值, 以后谁改了模板都对得上。 */
    lv_obj_set_align(panel, is_me ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(panel, lv_color_hex(is_me ? 0x35D28D : 0x2F2F30), 0);
    lv_obj_set_style_bg_opa(panel, 255, 0);
    lv_obj_set_style_border_opa(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, 8, 0);
    lv_obj_set_style_pad_right(panel, 8, 0);
    lv_obj_set_style_pad_top(panel, 6, 0);
    lv_obj_set_style_pad_bottom(panel, 3, 0);    /* 模板从 6 改成了 3 */
    /* ★ 这里【故意不设 radius】—— 和 SquareLine 导出的 ui_mepanel 一致: 样例也没设,
     *   走主题默认 (lv_theme_default.c:27 的 RADIUS_DEFAULT, 随 DPI 缩放)。
     *   自己写死一个数就和 SquareLine 预览不一致了。 */

    /* ---- 3. 文字 (默认已是 LV_LABEL_LONG_WRAP, 见 lv_label.c:748) ---- */
    lv_obj_t *label = lv_label_create(panel);
    lv_obj_set_size(label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    /* ★ 换行靠这条: 宽度到 240 就再也长不动 → WRAP 折行。
     *   和模板的 ui_metext / ui_restext 同一个值 (原来是 280)。
     *   ⚠️ 改模板里的 max_width 时这里要跟着改, 否则屏幕上的气泡和 SquareLine 预览宽度不一致。 */
    lv_obj_set_style_max_width(label, 240, 0);
    lv_obj_set_style_text_font(label, &ui_font_ch14, 0);
    /* 我的: 黑字配绿底; 对方: 白字配灰底 (和 ui_restext 一致) */
    lv_obj_set_style_text_color(label, lv_color_hex(is_me ? 0x000000 : 0xFFFFFF), 0);
    /* ★ 文字放 PSRAM → 所有权归我们, 标签被删时必须回收 */
    lv_obj_add_event_cb(label, label_deleted_cb, LV_EVENT_DELETE, NULL);
    label_set(label, text);

    return label;
}

/* 新建并登记一条消息, 超限删最老的。**同时写进环形缓冲** (重放时要用)。
 * ★ why 只用于日志 —— 标出【是哪条路径】建的气泡, 排查"多出一个气泡"时看它就够了。
 * ★ out_slot: 回传这条在环形缓冲里的槽位号 (流式刷新时要去改它); 可传 NULL。 */
static lv_obj_t *push(const char *text, bool is_me, const char *why, int *out_slot)
{
    if (s_bubble_cnt >= CHAT_MAX_BUBBLES) {
        /* 删 wrapper → 连带删气泡和标签 (LVGL 是树形, 删父删子) */
        lv_obj_del(s_bubbles[0]);
        memmove(s_bubbles, s_bubbles + 1,
                sizeof(s_bubbles[0]) * (CHAT_MAX_BUBBLES - 1));
        s_bubble_cnt--;
    }

    lv_obj_t *label = add_bubble(text, is_me);
    lv_obj_t *roll  = lv_obj_get_parent(lv_obj_get_parent(label));   /* label→panel→roll */
    s_bubbles[s_bubble_cnt++] = roll;

    ESP_LOGI(TAG, "[气泡] 新建 #%d %s (原因: %s) text=\"%.24s\"",
             s_bubble_cnt - 1, is_me ? "我" : "对方", why, (text != NULL) ? text : "");

    /* 新气泡的坐标要等布局算完才有 → 先更新布局再滚。
     * (lv_obj_update_layout 重入时 early return, 见 lv_obj_pos.c:383 —— 不会死锁) */
    lv_obj_update_layout(ui_contextPanel);
    lv_obj_scroll_to_view(roll, LV_ANIM_ON);

    if (out_slot != NULL) {
        *out_slot = ring_append(text, is_me);   /* ★ 同时进历史缓冲 */
    }

    return label;
}

/* 把环形缓冲里的历史按【从老到新】重建成气泡。
 *
 * ⚠️ 当前导出下它**几乎不会被用到**: 屏幕不销毁 → 气泡一直在 → 只在开机第一次
 *    由 sync_screen 调一次, 而那时缓冲是空的, 函数体直接 return。
 *    留着是因为它便宜, 而且万一 UI 又改回"离开即销毁"就立刻有用。
 *
 * ★ 用 add_bubble + 手动登记, **不能调 push()** —— push 会再往缓冲里写一遍,
 *   重放就变成滚雪球了。
 * ★ 不逐条 scroll_to_view: 10 条会滚 10 次, 肉眼可见地一顿一顿。最后滚一次就够。 */
static void ring_replay(void)
{
    if (s_ring_cnt == 0) {
        return;
    }

    /* 缓冲是环形的 → 有效数据从 (head + SLOTS - cnt) % SLOTS 开始 */
    int start = (s_ring_head + CHAT_RING_SLOTS - s_ring_cnt) % CHAT_RING_SLOTS;

    int made = 0;
    for (int k = 0; k < s_ring_cnt; k++) {
        const ring_slot_t *s = &s_ring[(start + k) % CHAT_RING_SLOTS];
        /* ★ 跳过空槽和空串: 空气泡(按了没说)在收尾时被 ring_update 清成 ""
         *   —— 它不该在历史里留个莫名其妙的小绿块。 */
        if (s->text == NULL || s->text[0] == '\0') {
            continue;
        }

        if (s_bubble_cnt >= CHAT_MAX_BUBBLES) {   /* 历史比屏幕能放的还多 → 淘汰最老的 */
            lv_obj_del(s_bubbles[0]);
            memmove(s_bubbles, s_bubbles + 1,
                    sizeof(s_bubbles[0]) * (CHAT_MAX_BUBBLES - 1));
            s_bubble_cnt--;
        }
        lv_obj_t *label = add_bubble(s->text, s->is_me);
        s_bubbles[s_bubble_cnt++] = lv_obj_get_parent(lv_obj_get_parent(label));
        made++;
    }

    if (made > 0) {
        lv_obj_update_layout(ui_contextPanel);
        lv_obj_scroll_to_view(s_bubbles[s_bubble_cnt - 1], LV_ANIM_OFF);   /* 一次滚到底 */
        ESP_LOGI(TAG, "聊天屏重建, 重放 %d 条历史", made);
    }
}

/* 上屏前同步屏幕状态。返回 false = 拿不到可用的父对象, 现在不能上屏。
 *
 * ★★ 这里的分支是"屏幕被销毁重建"时代的产物, 现在的导出【已经不是那个行为了】——
 *   2026-xx 重新导出后有两处变化 (都要记住, 否则会误判):
 *
 *   ① `ui_init()` 就把聊天屏建好了 (ui.c:549 `ui_chat_screen_init()`),
 *      不再是从 home 右转时才懒加载 → **ui_contextPanel 从开机起就非 NULL**
 *   ② 所有屏幕都没有 `scr_unloaded_delete_cb` 绑定了 (LVGL 9 导出里 SquareLine 没给),
 *      → **离开聊天屏【不再销毁】→ ui_contextPanel 指针永远不变**
 *
 *   ⇒ 原设计靠"指针为 NULL"识别"不在聊天屏"、靠"指针变了"识别"屏幕重建", 两个判据全失效:
 *      · `== NULL` 这条分支实际上【永远不会进】(留着只是为了以后 UI 又改回去时兜底)
 *      · `s_ctx_hooked != ui_contextPanel` 只在【开机第一次】成立一次, 之后不再触发
 *
 *   ⇒ 由此得到一个副产品 (是好事): 面板一直在 → 气泡也一直在 → 换屏回来消息不丢,
 *      所以"重放历史"基本用不上了 (ring_replay 在开机那次跑, 缓冲是空的, 无操作)。
 *      环形缓冲现在只是【万一以后屏幕又开始销毁】的保险。
 *
 *   ⚠️ 也正因为面板一直存在, 上屏函数**不再区分在不在聊天屏** ——
 *      在别的屏时收到消息也会正常建气泡 (看不见, 但回到聊天屏就在)。
 *      这是有意的: 用户不在聊天屏时说的话/收到的回复不该丢。
 *
 *   ⚠️ 绝不能拿 NULL 去 lv_obj_create —— 那会创建【一个新的屏幕】而不是子对象。 */
bool sync_screen(void)
{
    if (ui_contextPanel == NULL) {
        /* 兜底: 面板真的不存在 (只有旧式"离开即销毁"的导出才会走到)。
         * ★ 只清【气泡指针】, 环形缓冲保留 —— 它就是要跨屏幕重建活下来的。 */
        s_bubble_cnt    = 0;
        s_live_label    = NULL;
        s_live_res      = NULL;
        s_live_slot_me  = -1;
        s_live_slot_res = -1;
        s_ctx_hooked    = NULL;
        s_end_ms        = 0;
        s_asr_active    = false;
        return false;
    }

    if (s_ctx_hooked != ui_contextPanel) {
        /* 开机第一次 (或将来屏幕真被重建了) → 旧指针全部作废 */
        if (s_ctx_hooked != NULL) {
            ESP_LOGW(TAG, "聊天屏被重建, 清空气泡记录");
        }
        s_ctx_hooked    = ui_contextPanel;
        s_bubble_cnt    = 0;
        s_live_label    = NULL;
        s_live_res      = NULL;
        s_live_slot_me  = -1;
        s_live_slot_res = -1;
        s_end_ms        = 0;
        s_asr_active    = false;

        /* 删掉 SquareLine 预置的占位气泡("你好"/"hello"), 否则永远和真实消息混在一起。
         * ★ lv_obj_is_valid 只做指针比较、不解引用入参 (lv_obj.c:450) → 悬空也安全。
         * ★ 删完立刻置 NULL, 免得 SquareLine 的 destroy 或别处再引用。 */
        if (ui_merollpanel != NULL && lv_obj_is_valid(ui_merollpanel)) {
            lv_obj_del(ui_merollpanel);
        }
        ui_merollpanel = NULL;
        if (ui_resrollpanel != NULL && lv_obj_is_valid(ui_resrollpanel)) {
            lv_obj_del(ui_resrollpanel);
        }
        ui_resrollpanel = NULL;

        /* ★★ 把说过的话重新摆回来 —— 这个分支只在指针变了时进一次, 不会每轮重放。
         *    开机那次缓冲是空的 → 直接返回, 无副作用。 */
        ring_replay();
    }

    /* ③ 兜底: 第一个气泡已不合法 → 整体作废 */
    if (s_bubble_cnt > 0 && !lv_obj_is_valid(s_bubbles[0])) {
        ESP_LOGW(TAG, "气泡指针已失效, 清空气泡记录");
        s_bubble_cnt    = 0;
        s_live_label    = NULL;
        s_live_res      = NULL;
        s_live_slot_me  = -1;
        s_live_slot_res = -1;
        s_end_ms        = 0;
    }
    return true;
}

/* 收尾当前气泡: 空的直接删掉(按了没说 / 服务器没回字), 非空的只释放指针
 * (它已经是一条普通的历史消息了)。 */
static void close_bubble(void)
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

        /* ★ 空气泡不该留在历史里 —— 否则下次进屏会重放出一个莫名其妙的小绿块。
         *   把槽位文字清空, ring_replay 会跳过空文字。 */
        ring_update(s_live_slot_me, "");
    }
    s_live_label   = NULL;
    s_live_slot_me = -1;
}

/* 收到一条 ASR 文本 → 上屏 (由 drain_queues 在 lvgl 任务里调用) */
void show_asr(const char *text, bool is_final)
{
    if (text == NULL) {
        return;
    }
    if (!sync_screen()) {
        /* 拿不到 ui_contextPanel (理论上只有 ui_init 之前才可能) → 这条上屏不了。 */
        ESP_LOGW(TAG, "[上屏] 聊天面板不存在, 丢弃显示: %s", text);
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
        label_set(s_live_label, text);
        ring_update(s_live_slot_me, text);      /* ★ 历史里那条也要跟着走, 否则重放出来是残句 */
        lv_obj_update_layout(ui_contextPanel);
        /* ★ 用 ANIM_OFF: 流式期每 ~137ms 一条, 开动画会一直重启滚动看着抖 */
        lv_obj_scroll_to_view(lv_obj_get_parent(lv_obj_get_parent(s_live_label)), LV_ANIM_OFF);
    } else if (s_asr_active) {
        s_live_label = push(text, true, "会话进行中补建(中途才切到聊天屏)", &s_live_slot_me);
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
void begin_asr(void)
{
    if (!sync_screen()) {
        ESP_LOGW(TAG, "[上屏] 会话开始, 但聊天面板不存在 —— 不出气泡");
        return;
    }


    close_bubble();   /* ★ 上一轮若还挂着(宽限期内又按了), 先结掉再建新的 */
    s_end_ms     = 0;
    s_asr_active = true;
    s_live_label = push("", true, "按下键 STARTED", &s_live_slot_me);
}

/* 收到一段 LLM 回复 → 写进"对方"的气泡 (灰底 / 靠左)。
 * ★ 和 ASR 那边的区别: 一次 chat 只产生一条 reply, 没有"多段"问题,
 *   所以 is_final 时直接释放指针即可 (下一条回复会新建气泡)。 */
void show_llm(const char *text, bool is_final)
{
    if (text == NULL) {
        return;
    }
    if (!sync_screen()) {
        ESP_LOGW(TAG, "[上屏] 聊天面板不存在, LLM 回复不显示: %s", text);
        return;
    }

    if (s_live_res != NULL) {
        label_set(s_live_res, text);           /* 复用同一条, 只改文字 */
        ring_update(s_live_slot_res, text);    /* ★ 历史里那条也要跟着走 */
        lv_obj_update_layout(ui_contextPanel);
        lv_obj_scroll_to_view(lv_obj_get_parent(lv_obj_get_parent(s_live_res)), LV_ANIM_OFF);
    } else {
        s_live_res = push(text, false, "LLM 回复", &s_live_slot_res);
    }

    if (is_final) {
        ESP_LOGI(TAG, "[上屏] LLM 回复完成");
        s_live_res     = NULL;                     /* 下一条回复新建气泡 */
        s_live_slot_res = -1;
    }
}

/* 会话结束 (松开 / 中止) → **只记时刻, 不立刻收尾**。
 * ★ 松手后 voice_task 立刻投 ENDED, 而服务器随后(~400ms)才发 final ——
 *   那条 final 是最终修正版, 必须还能落进同一条气泡。
 *   真正的收尾交给 tick (宽限期到期) 或下一次 begin_asr。 */
void end_asr(void)
{
    s_asr_active = false;
    if (s_live_label != NULL) {
        s_end_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }
    /* ★ "要不要去问 openclaw" 不属于本模块的职责 —— 由 ask_router::on_session_end()
     *   在 drain_queues 里紧接着调用 (见那里的注释)。 */
}

/* 每轮调一次 (drain_queues 里): 宽限期过了就真收尾。
 * 这样"按了没说"的空气泡不会一直挂着, 而 final 也来得及落进来。 */
void tick(void)
{
    if (s_end_ms == 0 || s_live_label == NULL) {
        return;
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if ((now - s_end_ms) < CHAT_END_GRACE_MS) {
        return;                 /* 还在等 final */
    }
    close_bubble();
    s_end_ms = 0;
}

}  // namespace chat_view

