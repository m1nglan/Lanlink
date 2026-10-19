#include "display/msg_panel.hpp"

#include <string.h>                /* strcmp / strlcpy */

#include "esp_log.h"
#include "lvgl.h"

#include "ui.h"                    /* ui_messagepanel / ui_clawmesspanel / ... / openmessage_Animation */
#include "ui_helpers.h"            /* _ui_screen_change */
#include "display/lvgl_port.hpp"   /* lvgl_port_cho_enter / cho_exit / cho_focused */

static const char *TAG = "msg_panel";

namespace msg_panel {

/*! 面板展开宽度 —— 和 SquareLine 里 ui_messagepanel 的宽一致 (openmessage 动画到 314) */
#define PANEL_W_OPEN   (314)

static bool s_open        = false;
static int  s_last_entry  = ENTRY_DEEP;   /*!< 最近一次写入的是哪一条 */
static char s_last_text[256] = "";        /*!< 上次写下去的文字 (变化检测) */

static bool ok(lv_obj_t *o)
{
    return (o != nullptr) && lv_obj_is_valid(o);
}

/* ★ 前置声明: 定义在下面, 但 has_entry() 要用它。 */
static lv_obj_t *visible_entry(void);

/*! 把两条互斥地切一下: 只显示 entry 对应那条, 另一条隐藏。 */
static void show_entry(int entry)
{
    if (ok(ui_clawmesspanel)) {
        if (entry == ENTRY_CLAW) {
            lv_obj_remove_flag(ui_clawmesspanel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui_clawmesspanel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (ok(ui_deepmesspanel)) {
        if (entry == ENTRY_DEEP) {
            lv_obj_remove_flag(ui_deepmesspanel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui_deepmesspanel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* "没有消息~" 只在两条都没有内容时显示 → 有正文就藏起来 */
    if (ok(ui_nomessagetext)) {
        lv_obj_add_flag(ui_nomessagetext, LV_OBJ_FLAG_HIDDEN);
    }
}

void init(void)
{
    s_open       = false;
    s_last_entry = ENTRY_DEEP;
    s_last_text[0] = '\0';

    /* ★ SquareLine 导出时面板是【展开】的 (width 314), 开机必须收起。
     *   直接置 0, 不播动画 (开机不该有这动静)。 */
    if (ok(ui_messagepanel)) {
        lv_obj_set_width(ui_messagepanel, 0);
    }
    if (ok(ui_clawmesspanel)) {
        lv_obj_add_flag(ui_clawmesspanel, LV_OBJ_FLAG_HIDDEN);
    }
    if (ok(ui_deepmesspanel)) {
        lv_obj_add_flag(ui_deepmesspanel, LV_OBJ_FLAG_HIDDEN);
    }
    if (ok(ui_nomessagetext)) {
        lv_obj_remove_flag(ui_nomessagetext, LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGI(TAG, "通知面板就绪 (收起, 无通知)");
}

bool is_open(void)
{
    return s_open;
}

bool can_open(void)
{
    /* 只有 home 屏允许展开通知面板 (通知面板挂在 ui_home 上) */
    return (ui_home != nullptr) && (lv_screen_active() == ui_home);
}

bool has_entry(void)
{
    return visible_entry() != nullptr;
}

void set_reply_text(const char *text, bool is_claw)
{
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    /* ★ 值没变就别写 —— 流式时同一段文字可能重复到达, 白重排一次不划算 */
    if (strcmp(text, s_last_text) == 0 && (is_claw == (s_last_entry == ENTRY_CLAW))) {
        return;
    }
    strlcpy(s_last_text, text, sizeof(s_last_text));
    s_last_entry = is_claw ? ENTRY_CLAW : ENTRY_DEEP;

    lv_obj_t *lab = is_claw ? ui_clawanstext : ui_deepanstext;
    if (ok(lab)) {
        lv_label_set_text(lab, text);
    }
    show_entry(s_last_entry);
}

int focused_entry(void)
{
    if (!s_open) {
        return s_last_entry;
    }
    lv_obj_t *f = (lv_obj_t *)lvgl_port_cho_focused();
    if (f == ui_clawmesspanel) {
        return ENTRY_CLAW;
    }
    if (f == ui_deepmesspanel) {
        return ENTRY_DEEP;
    }
    return s_last_entry;
}

/*! 当前可见的那一条 (只有一条会显示; 都没有则返回 NULL)。 */
static lv_obj_t *visible_entry(void)
{
    if (ok(ui_clawmesspanel) && !lv_obj_has_flag(ui_clawmesspanel, LV_OBJ_FLAG_HIDDEN)) {
        return ui_clawmesspanel;
    }
    if (ok(ui_deepmesspanel) && !lv_obj_has_flag(ui_deepmesspanel, LV_OBJ_FLAG_HIDDEN)) {
        return ui_deepmesspanel;
    }
    return nullptr;
}

void open(void)
{
    if (s_open || !ok(ui_messagepanel)) {
        return;
    }
    s_open = true;

    /* ① 宽度归 0 再播展开动画 —— 动画起点是写死的 0 (ui.c 的 openmessage_Animation),
     *    不复位的话会看到"先跳宽再展示"的一下。 */
    lv_obj_set_width(ui_messagepanel, 0);
    /* ② openmessage_Animation: 宽度 0 → 314, 500ms, ease_out */
    openmessage_Animation(ui_messagepanel, 0);

    /* ③ 编码器只在【已显示的那一条】上移动焦点。
     *   ★ 之前把两条都加进组 → 隐藏那条也在组里, 编码器会跳到一个看不见的格子上 ✗
     *   ★ 只有一条可见时就只放一条, 另一格传 NULL (lvgl_port_cho_enter 会自动跳过)。
     *   ⚠️ 代价: 只有一条通知时编码器转不动 (组里就一个对象) —— 这是符合预期的,
     *     因为另一条根本没内容。 */
    lv_obj_t *vis = visible_entry();
    lvgl_port_cho_enter(vis, nullptr, vis);
    ESP_LOGI(TAG, "通知面板展开 (可见条目: %s)",
             (vis == ui_clawmesspanel) ? "明岚" : (vis == ui_deepmesspanel) ? "DeepSeek" : "无");
}

void close(void)
{
    if (!s_open) {
        return;
    }
    s_open = false;
    lvgl_port_cho_exit();
    if (ok(ui_messagepanel)) {
        /* ★ 收起走 SquareLine 的 closemessage_Animation (ui.c):
         *      宽度 314 → 0, 300ms, ease_out
         *   先把可能还在跑的展开动画删掉, 否则两条同时改宽度会打架 ✗
         *   ⚠️ 必须用 NULL 通配 —— SquareLine 生成的回调签名是 void(*)(lv_anim_t*, int32_t),
         *      不是 lv_anim_exec_xcb_t, 传不进 lv_anim_delete ✗ */
        lv_anim_delete(ui_messagepanel, NULL);
        closemessage_Animation(ui_messagepanel, 0);
    }
    ESP_LOGI(TAG, "通知面板收起");
}

void enter_chat(void)
{
    /* 从 home 往右进聊天屏 —— 和 SquareLine 里 ui_home 的 MOVE_LEFT 是同一套动画感 */
    _ui_screen_change(&ui_chat, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 500, 0, &ui_chat_screen_init);
    ESP_LOGI(TAG, "切到聊天屏");
}

void mark_read(void)
{
    /* ★ 只在 chat 屏生效 —— 调用方 (ui_bridge) 每 50ms 无条件调一次,
     *   这里自己判断, 免得业务层认识 ui_chat 这种屏符号 ✓ */
    if (lv_screen_active() != ui_chat) {
        return;
    }
    /* 已经处理完 (面板关着 + 没有正文 + "没有消息~" 已显示) → 直接返回, 不重复操作 */
    if (!s_open && s_last_text[0] == '\0'
        && ok(ui_nomessagetext) && !lv_obj_has_flag(ui_nomessagetext, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    if (s_open) {
        close();
    }
    s_last_text[0] = '\0';

    if (ok(ui_clawmesspanel)) {
        lv_obj_add_flag(ui_clawmesspanel, LV_OBJ_FLAG_HIDDEN);
    }
    if (ok(ui_deepmesspanel)) {
        lv_obj_add_flag(ui_deepmesspanel, LV_OBJ_FLAG_HIDDEN);
    }
    /* 清空正文, 免得下次展开还看到旧内容 */
    if (ok(ui_clawanstext)) {
        lv_label_set_text(ui_clawanstext, "");
    }
    if (ok(ui_deepanstext)) {
        lv_label_set_text(ui_deepanstext, "");
    }
    /* ★ 解除"没有消息~"的隐藏 */
    if (ok(ui_nomessagetext)) {
        lv_obj_remove_flag(ui_nomessagetext, LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGI(TAG, "通知已读: 面板收起 + 正文清空 + 显示\"没有消息~\"");
}

}  // namespace msg_panel
