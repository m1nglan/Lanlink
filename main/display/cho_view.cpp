#include "display/cho_view.hpp"

#include "esp_log.h"
#include "lvgl.h"
#include "nvs.h"                   /* 持久化: 记住上次选的模型 */

#include "ui.h"                    /* ui_choPanel / ui_choclaw2 / ui_chodeep2 */
#include "display/lvgl_port.hpp"   /* lvgl_port_cho_enter / cho_exit / cho_focused */

static const char *TAG = "cho_view";

/* ---- 持久化 ----
 * ★ 用 NVS 默认分区, 命名空间 "lanlink", 键 "model": 0=明岚 1=DeepSeek。
 * ⚠️ 依赖 nvs_flash_init() 已经跑过 —— main.cpp 在 cho_view::init() 之前会调
 *    (wifi.cpp 里也调, 那个是幂等的)。
 * ⚠️ 读失败 / 键不存在 → 用默认值, 不算错误 (第一次开机就是这情况)。 */
#define NVS_NS    "lanlink"
#define NVS_KEY   "model"

namespace cho_view {

/*! 焦点环的配色 —— 直接照抄 SquareLine 里 ui_choclaw2 那套"选中态"模板:
 *      outline_color 0x70C0F1 (浅蓝) / width 2 / pad 2 / opa 255
 *  再把"什么时候可见"从 DEFAULT 状态挪到 FOCUSED 状态, 就能让环跟着焦点走:
 *      DEFAULT → opa 0    (平时两个图标都不显环)
 *      FOCUSED → opa 255  (只有聚焦的那个显)
 *  ★ 为什么必须自己再设一遍而不是直接用导出的: SquareLine 把 opa=255 写在了
 *    DEFAULT 上 (ui_choclaw2), 另一个写了 opa=0 (ui_chodeep2) —— 两个固定死,
 *    不会跟着焦点变 ✗ 所以这里用 LV_STATE_DEFAULT 覆盖掉它们。 */
#define CHO_RING_COLOR  0x70C0F1
#define CHO_RING_WIDTH  2
#define CHO_RING_PAD    2

static int  s_selected = CHOICE_DEEP;   /*!< 当前选中 (面板开着时 = 焦点项)。
                                         * ★ 默认必须和 ask_router 的 s_service 初值对齐:
                                         *   那边是 "llm" (DeepSeek), 所以这里也是 DEEP。 */
static bool s_open     = false;

/*! 从 NVS 读上次的选择。读不到就用默认值 (不算错误)。 */
static void nvs_load(void)
{
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "NVS 未就绪或命名空间不存在 (%s), 用默认: %s",
                 esp_err_to_name(err), s_selected == CHOICE_DEEP ? "DeepSeek" : "明岚");
        return;
    }
    uint8_t v = 0;
    err = nvs_get_u8(h, NVS_KEY, &v);
    nvs_close(h);
    if (err == ESP_OK && (v == CHOICE_CLAW || v == CHOICE_DEEP)) {
        s_selected = (int)v;
        ESP_LOGI(TAG, "从 NVS 恢复模型: %s", v == CHOICE_DEEP ? "DeepSeek" : "明岚");
    } else {
        ESP_LOGI(TAG, "NVS 里还没有模型记录 (%s), 用默认: %s",
                 esp_err_to_name(err), s_selected == CHOICE_DEEP ? "DeepSeek" : "明岚");
    }
}

/*! 把当前选择写回 NVS。失败只告警 —— 不影响本次生效, 只是下次开机记不住。 */
static void nvs_save(void)
{
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 打开失败 (%s), 选择未能持久化", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(h, NVS_KEY, (uint8_t)s_selected);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 写入失败 (%s), 选择未能持久化", esp_err_to_name(err));
    }
}

static bool obj_ok(lv_obj_t *o)
{
    return (o != nullptr) && lv_obj_is_valid(o);
}

/*! 给一个图标套上"焦点环"样式 (见 CHO_RING_* 的说明)。 */
static void apply_focus_style(lv_obj_t *o)
{
    if (!obj_ok(o)) {
        return;
    }
    /* 平时不显形 —— 盖掉 SquareLine 写死的 255/0 */
    lv_obj_set_style_outline_opa(o, 0, LV_STATE_DEFAULT);

    /* 聚焦时才显形 (属性继承自 DEFAULT, 这里只把 opa 打开) */
    lv_obj_set_style_outline_color(o, lv_color_hex(CHO_RING_COLOR), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(o, CHO_RING_WIDTH, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(o, CHO_RING_PAD, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_opa(o, 255, LV_STATE_FOCUSED);
}

void init(void)
{
    nvs_load();                    /* ★ 先恢复上次的选择, 再套样式/收面板 */

    apply_focus_style(ui_choclaw2);
    apply_focus_style(ui_chodeep2);

    /* ★ SquareLine 导出时面板是【展开】的 (height 100, 无 HIDDEN),
     *   开机必须收起来 —— 直接 set_height(0), 不播关闭动画 (开机不该有这动静)。 */
    if (obj_ok(ui_choPanel)) {
        lv_obj_set_height(ui_choPanel, 0);
    }
    s_open = false;
    ESP_LOGI(TAG, "模型面板就绪 (当前模型: %s)", s_selected == CHOICE_DEEP ? "DeepSeek" : "明岚");
}

bool is_open(void)
{
    return s_open;
}

bool can_open(void)
{
    /* 只有 chat 屏允许开面板 —— 别的屏按 IO2 不动作。
     * ⚠️ ui_chat 是 lv_obj_create(NULL) 建的"屏对象", lv_screen_active() 返回的也是屏,
     *    所以指针比较是合法的。 */
    return (ui_chat != nullptr) && (lv_screen_active() == ui_chat);
}

int selected(void)
{
    return s_selected;
}

void set_selected(int choice)
{
    if (choice != CHOICE_CLAW && choice != CHOICE_DEEP) {
        return;
    }
    const bool changed = (choice != s_selected);
    s_selected = choice;
    if (changed) {
        nvs_save();                /* ★ 只在真的变了才写 NVS (保护 flash 寿命) */
    }
}

void open(void)
{
    if (s_open || !obj_ok(ui_choPanel)) {
        return;
    }
    s_open = true;

    /* ① 先让面板可见并归零高度, 否则动画从 100 起跳会有"闪一下" */
    lv_obj_remove_flag(ui_choPanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(ui_choPanel, 0);

    /* ② 播展开动画 (ui.c:463, height 0 → 100, 600ms, ease_out) */
    choosemode_Animation(ui_choPanel, 0);

    /* ③ 编码器改为"在两项间移动焦点" (导航模式), 焦点落到当前选中项 */
    lvgl_port_cho_enter(ui_choclaw2, ui_chodeep2,
                        (s_selected == CHOICE_CLAW) ? (void *)ui_choclaw2 : (void *)ui_chodeep2);

    ESP_LOGI(TAG, "面板展开");
}

void close(void)
{
    if (!s_open) {
        return;
    }
    s_open = false;

    /* ① 先把编码器交还给屏幕 —— 顺序很重要: 否则收起动画期间还能转焦点,
     *    焦点已经出了面板, 下一轮 lvgl_focus_active_screen_locked 又会打架。 */
    lvgl_port_cho_exit();

    /* ② 收起动画 (ui.c:529, height 100 → 0, 300ms)。
     *    ⚠️ 动画结束后要重新 add_flag(HIDDEN), 否则面板虽然高度 0, 但它的子控件
     *      仍参与布局/命中测试。用 lv_anim 的回调做不到 (那是 SquareLine 生成的
     *      anim, 我们拿不到它的 completed 回调) → 所以这里用一个 lv_timer 在动画
     *      时长之后补一刀。 */
    if (obj_ok(ui_choPanel)) {
        chomodelclose_Animation(ui_choPanel, 0);
        /* 350ms = 动画 300ms + 50ms 余量 */
        lv_timer_t *t = lv_timer_create([](lv_timer_t *tt) {
            if (obj_ok(ui_choPanel)) {
                lv_obj_add_flag(ui_choPanel, LV_OBJ_FLAG_HIDDEN);
            }
            lv_timer_delete(tt);   /* ★ 从自己的回调里删自己是安全的 (lv_timer_handler 有 timer_deleted 处理) */
        }, 350, nullptr);
        if (t == nullptr) {
            /* 建 timer 失败也别把面板留在半开状态 */
            lv_obj_add_flag(ui_choPanel, LV_OBJ_FLAG_HIDDEN);
        }
    }

    ESP_LOGI(TAG, "面板收起");
}

/*! 内部用: 面板开着时, 从焦点反推选中项。给 UiBridge 确认时调。 */
int focused_choice(void)
{
    /* ★ 必须显式转: lvgl_port.hpp 故意不 include lvgl.h (保持头文件轻),
     *   所以那几个接口用 void* 传对象指针; C++ 不会把 void* 隐式转成 lv_obj_t* ✗ */
    lv_obj_t *f = (lv_obj_t *)lvgl_port_cho_focused();
    if (f == ui_chodeep2) {
        return CHOICE_DEEP;
    }
    if (f == ui_choclaw2) {
        return CHOICE_CLAW;
    }
    return s_selected;
}

}  // namespace cho_view
