#include "display/msg_ball.hpp"

#include "esp_log.h"
#include "lvgl.h"

#include "ui.h"          /* ui_messball / ui_nomessball / ui_chat */

static const char *TAG = "msg_ball";

namespace msg_ball {

/*! 是否有未读消息 (只在这一个 bool 上判断, 不重复操作控件 → 不闪) */
static bool s_unread = false;

static bool ok(lv_obj_t *o)
{
    return (o != nullptr) && lv_obj_is_valid(o);
}

/* ================================================================
 * 面板宽度 —— 两个状态用两个固定值 (ui_messballpanel 在 SquareLine 里是固定宽, 不再是
 * LV_SIZE_CONTENT, 所以代码可以直接改)。
 *
 *   PANEL_W_WIDE (82): 无未读 —— 装得下"没有消息"那段文字
 *   PANEL_W_NARROW(32): 有未读 —— 只装一个 24×24 的红点球
 *
 * ★ 收到消息那一下走 SquareLine 的 newmessage_Animation (ui.c:554):
 *     宽度 82 → 32, 500ms, ease_out  —— 就是"胶囊收缩成一个小圆球"的效果
 * ★ 已读那一下【不播动画】, 直接把宽度设回 82 (用户定的)。
 * ================================================================ */
#define PANEL_W_WIDE    (82)
#define PANEL_W_NARROW  (32)

/*! 切到"有未读": messball 显 / nomessball 隐, 并播【收缩】动画 (82 → 32) */
static void show_ball(void)
{
    if (ok(ui_nomessball)) {
        lv_obj_add_flag(ui_nomessball, LV_OBJ_FLAG_HIDDEN);
    }
    if (ok(ui_messball)) {
        lv_obj_remove_flag(ui_messball, LV_OBJ_FLAG_HIDDEN);
    }
    if (ok(ui_messballpanel)) {
        /* ★ 动画的起点是写死的 82 (ui.c:554 的 lv_anim_set_values(82, 32))。
         *   所以调用前必须保证面板真的在 82 上 —— 否则会看到"先跳宽再收缩"的一下。
         *   正常流程里无未读时面板就是 82, 这里复位是为了扛住
         *   "上一次动画没播完就又被置了未读"之类的意外。 */
        lv_obj_set_width(ui_messballpanel, PANEL_W_WIDE);
        newmessage_Animation(ui_messballpanel, 0);   /* 82 → 32, 500ms, ease_out */
    }
}

/*! 切到"无未读": messball 隐 / nomessball 显, 宽度【直接】回到 82 (不播动画) */
static void show_nomess(void)
{
    if (ok(ui_messball)) {
        lv_obj_add_flag(ui_messball, LV_OBJ_FLAG_HIDDEN);
    }
    if (ok(ui_nomessball)) {
        lv_obj_remove_flag(ui_nomessball, LV_OBJ_FLAG_HIDDEN);
    }
    if (ok(ui_messballpanel)) {
        /* ★ 直接设宽, 不播动画 (用户定的: 已读是"点开就看见了", 不需要过渡)。
         *   但先得掐掉可能还在跑的收缩动画 —— 否则它会把宽度又拉回 32 ✗。
         *   ⚠️ 必须用 NULL 通配而不能传 _ui_anim_callback_set_width:
         *      SquareLine 生成的那个回调签名是 void(*)(lv_anim_t*, int32_t),
         *      不是 lv_anim_exec_xcb_t 的 void(*)(void*, int32_t) → 类型不匹配 ✗
         *      NULL 表示"这个对象上的所有动画" —— 面板上只有这一个动画, 安全 ✓ */
        lv_anim_delete(ui_messballpanel, NULL);
        lv_obj_set_width(ui_messballpanel, PANEL_W_WIDE);
    }
}

void init(void)
{
    s_unread = false;
    show_nomess();
    ESP_LOGI(TAG, "消息提示球就绪 (无未读)");
}

void notify_incoming(void)
{
    /* ★ 用户已经在聊天屏看着 → 不需要红点 */
    if (lv_screen_active() == ui_chat) {
        return;
    }
    /* ★ 已经是未读 → 幂等直接返回。
     *   这就是"不要重复创建, 只一个就行"的实现: 控件是 SquareLine 建的,
     *   我只切一次 HIDDEN 标志, 后续消息什么都不做 ✓ */
    if (s_unread) {
        return;
    }
    s_unread = true;
    show_ball();
    ESP_LOGI(TAG, "有新消息 → 显示提示球");
}

void update(void)
{
    if (!s_unread) {
        return;
    }
    if (lv_screen_active() != ui_chat) {
        return;
    }
    s_unread = false;
    show_nomess();
    ESP_LOGI(TAG, "已进入聊天屏 → 标记已读");
}

}  // namespace msg_ball
