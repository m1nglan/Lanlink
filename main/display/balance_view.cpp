#include "display/balance_view.hpp"

#include <stdio.h>         /* snprintf —— 必须是 libc 的, 不能用 LVGL 的 */
#include <math.h>          /* fabsf */

#include "esp_log.h"
#include "lvgl.h"

#include "ui.h"            /* ui_Balance / ui_balancenum */

static const char *TAG = "balance_view";

namespace balance_view {

/*! 上次写下去的值。-1 = "还没写过"/"缓存已作废"。
 *  ★ 必须是文件级 static: apply() 和 begin_refresh() 都要动它
 *    (放在 apply 里面的话 begin_refresh 访问不到 ✗) */
static float s_total = -1.0f;

bool is_active(void)
{
    return (ui_Balance != nullptr) && (lv_screen_active() == ui_Balance);
}

void begin_refresh(void)
{
    if (ui_balancenum == nullptr || !lv_obj_is_valid(ui_balancenum)) {
        return;
    }
    /* ★★ 作废"上次值"缓存 —— 这是关键。
     *   余额一般不变, apply() 里 `fabsf(total - s_total) < 0.005` 会判"没变化"直接
     *   return → 标签就永远停在 "--.--" ✗
     *   设成 -1 后, 下一次任何 total 都算"变了" → 一定会写回去 ✓ */
    s_total = -1.0f;

    /* 占位符用 "--.--": montserrat_38 是 ASCII 字体, '-' 和 '.' 都有 ✓
     * ⚠️ 不能用 "--" 或者 "…" 之类 —— 字体里没有的字符会显示成空白 */
    lv_label_set_text(ui_balancenum, "--.--");
    ESP_LOGI(TAG, "余额刷新中 (显示 --.--)");
}

void apply(float total)
{
    if (ui_balancenum == nullptr || !lv_obj_is_valid(ui_balancenum)) {
        return;
    }

    /* ★ 值没变就不写 —— 余额本来就不常变, 而 label 一写就要重排。
     *   用浮点比较: 差小于 0.005 元当作没变 (显示"%.2f"的精度就是分) ✓
     *   ⚠️ begin_refresh() 会把 s_total 打成 -1 → 刷新时一定会写回来 ✓ */
    if (s_total >= 0.0f && fabsf(total - s_total) < 0.005f) {
        return;
    }
    s_total = total;

    /* ★★ 必须用 libc 的 snprintf 格式化, 不能 lv_label_set_text_fmt("%.2f", ...) ✗
     *   LVGL 自带那个 lv_snprintf 默认【不支持 %f】(要开 LV_SPRINTF_USE_FLOAT),
     *   格式化失败的典型表现就是屏幕上只剩一个字面量 'f' ✗ (踩过)
     *
     * ★ 字体是 lv_font_montserrat_38 (ASCII) → 数字和 '.' 都有 ✓
     *   ¥ 由旁边的 ui_balicon 单独显示, 这里只写数字。 */
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", (double)total);
    lv_label_set_text(ui_balancenum, buf);
    ESP_LOGI(TAG, "余额: %s 元", buf);
}

}  // namespace balance_view
