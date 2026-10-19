#include "display/weather_view.hpp"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"

#include "ui.h"              /* ui_weather1 / ui_degree1 / ui_Weather_kind1 */

static const char *TAG = "weather_view";

/* ================================================================
 * 8 张天气图的 extern 声明 —— 全部自己声明，**不依赖 ui.h**
 *
 * ★★ 为什么：SquareLine 只把【当前挂在控件上的那一张】写进 ui.h。
 *    ui_weather1 一次只能挂一张，所以每导出一次，ui.h 里就只剩那一张，
 *    其余 7 张全掉出去 ✗ （实测：第一次是 cloudy、第二次是 night、第三次是 bloudy）
 *    但它们的 .c 文件一直在 main/lvgl/images/ 里（SquareLine 只写不删），
 *    而 main/CMakeLists.txt 的 SRC_DIRS 含 "lvgl/images" (file(GLOB)) → 都会被编译 ✓
 *    ⇒ 在代码里声明一次，以后无论怎么导出都不会断。
 *
 * ⚠️ 重复声明是合法的：ui.h 里已有的那些会和我这里形成两个等价的 extern 声明 —— 无害 ✓
 * ⚠️ 前提：.c 必须存在（下面 8 个都确认过）。
 * ================================================================ */
LV_IMG_DECLARE(ui_img_frame_00_png);    /*!< kind 1 白天晴 (太阳) */
LV_IMG_DECLARE(ui_img_night_png);       /*!< kind 2 夜晚晴 (月牙) */
LV_IMG_DECLARE(ui_img_cloudy_png);      /*!< kind 3 多云   (白云) */
LV_IMG_DECLARE(ui_img_bloudy_png);      /*!< kind 4 阴天   (深灰云) */
LV_IMG_DECLARE(ui_img_rain_1_png);      /*!< kind 5 雨     (云 + 雨滴) */
LV_IMG_DECLARE(ui_img_snow_4_png);      /*!< kind 6 雪     (云 + 雪花) */
LV_IMG_DECLARE(ui_img_lighting_1_png);  /*!< kind 7 雷雨   (云 + 闪电) */
LV_IMG_DECLARE(ui_img_wind_3_png);      /*!< kind 8 风     (风线) */

namespace weather_view {

/*! kind → 图片。未知一律兜底多云 (和网关 999→3 的约定一致)。 */
static const lv_image_dsc_t *kind_to_img(int32_t kind)
{
    switch (kind) {
    case weather::KIND_CLEAR_DAY:   return &ui_img_frame_00_png;     /* 白天晴: 太阳 */
    case weather::KIND_CLEAR_NIGHT: return &ui_img_night_png;        /* 夜晚晴: 月牙 */
    case weather::KIND_CLOUDY:      return &ui_img_cloudy_png;       /* 多云: 白云 */
    case weather::KIND_OVERCAST:    return &ui_img_bloudy_png;       /* 阴天: 深灰云 */
    case weather::KIND_RAIN:        return &ui_img_rain_1_png;       /* 雨: 云+雨滴 */
    case weather::KIND_SNOW:        return &ui_img_snow_4_png;       /* 雪: 云+雪花 */
    case weather::KIND_THUNDER:     return &ui_img_lighting_1_png;   /* 雷雨: 云+闪电 */
    case weather::KIND_WIND:        return &ui_img_wind_3_png;       /* 风: 风线 */
    default:                        return &ui_img_cloudy_png;       /* 未知 → 兜底多云 */
    }
}

void apply(const weather::info_t *w)
{
    if (w == nullptr) {
        return;
    }

    /* ---- 1. 天气图: 只在 kind 变了才换 (换图会触发重绘, 别每次都设) ---- */
    static int32_t s_last_kind = -1;
    if (w->kind != s_last_kind) {
        s_last_kind = w->kind;
        if (ui_weather1 != NULL && lv_obj_is_valid(ui_weather1)) {
            lv_image_set_src(ui_weather1, kind_to_img(w->kind));
            ESP_LOGI(TAG, "[天气] 图标 → kind=%d", (int)w->kind);
        }
    }

    /* ---- 2. 温度: 只写数字, 不带 ℃ (那个符号是旁边的 ui_degreeimage1 图标) ----
     * ⚠️ ui_degree1 的字体是 ui_font_date18, 里面【有 '-'】(已确认),
     *    所以零下能正常显示 "-2"。但【没有小数点】—— 若网关将来给出 "5.5"
     *    这类值, json_int() 会截成 5 (整数), 不会出错但会丢小数。 */
    static int32_t s_last_temp = INT32_MIN;
    if (w->temp != s_last_temp) {
        s_last_temp = w->temp;
        if (ui_degree1 != NULL && lv_obj_is_valid(ui_degree1)) {
            lv_label_set_text_fmt(ui_degree1, "%d", (int)w->temp);
        }
    }

    /* ---- 3. 天气文字 ----
     * ⚠️ ui_Weather_kind1 的字体 ui_font_weather18 是【子集】,
     *    目前覆盖: 晴多云少间阴阵雨强雷伴有冰雹小中大极端降毛细暴特冻到雪夹天气薄雾霾扬沙浮尘浓度重严热冷未知
     *    ★ 已知【缺 "风"】—— kind=8 时若 text 是 "风"/"大风"/"狂风",
     *      那个字会画成空白。需要在 SquareLine 里给这个字体补上 风 (和 狂)。
     *    ★ 缺字不会崩, 只是不显示 —— LVGL 对字体里没有的码点直接跳过。 */
    static char s_last_text[sizeof(w->text)] = "";
    if (strcmp(w->text, s_last_text) != 0) {
        strlcpy(s_last_text, w->text, sizeof(s_last_text));
        if (ui_Weather_kind1 != NULL && lv_obj_is_valid(ui_Weather_kind1)) {
            /* ★ 单个字 ("晴") 能不能居中, 取决于控件的宽度, 不是代码能决定的:
             *   ui_Weather_kind1 的宽度是 LV_SIZE_CONTENT + text_align = CENTER,
             *   而 LVGL 的文字居中只在【标签宽度 > 文字宽度】时才生效 ——
             *   宽度跟着内容走时没有富余, 居中是空操作。
             *   ⇒ 在 SquareLine 里给它设一个【最小宽度】(min_width, 比如 40~59),
             *     宽度就有下限了, 居中对任何字数都成立 ✓ 代码这边不用做任何补齐。 */
            lv_label_set_text(ui_Weather_kind1, w->text);
        }
    }
}

}  // namespace weather_view
