#include "display/server_view.hpp"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"

#include "ui.h"          /* ui_cpunum / ui_rambar / ... (都在 screens/ui_Server.h) */

static const char *TAG = "server_view";

namespace server_view {

static bool ok(lv_obj_t *o)
{
    return (o != nullptr) && lv_obj_is_valid(o);
}

/*! 速度标签格式化 —— 文本框只放得下"两位数字 + 单位"(如 "32kb") ✗
 *
 *   < 100 kb  → "%dkb"   最长 "99kb"     (4 字符) ✓
 *   ≥ 100 kb  → "%dmb"   这样单位变短, 两位数字照样放得下 ✓
 *               ⚠️ 用【向上取整】: 100..1023 kb 如果按 1/1024 取整会得到 0mb ✗
 *                  (显示 "0mb" 比"稍微高报一点"更糟)
 *  ★ 极端情况: ≥ 100 MB 会显示 "100mb"(5 字符), 可能撑出去 —— 家里这台服务器
 *    跑不到, 真出现了再改成 GB。 */
static void set_speed(lv_obj_t *lab, int16_t kb)
{
    if (!ok(lab)) {
        return;
    }
    if (kb < 100) {
        lv_label_set_text_fmt(lab, "%dkb", (int)kb);
    } else {
        const int mb = ((int)kb + 1023) / 1024;      /* 向上取整 */
        lv_label_set_text_fmt(lab, "%dmb", mb);
    }
}

/*! 三个百分比组共用的写法: 数字标签 + 进度条。值没变就不写。 */
static void set_pct(lv_obj_t *lab, lv_obj_t *bar, int16_t pct, int16_t *cache)
{
    if (*cache == pct) {
        return;                      /* 没变 → 一次都不碰 */
    }
    *cache = pct;
    if (ok(lab)) {
        lv_label_set_text_fmt(lab, "%d%%", (int)pct);
    }
    if (ok(bar)) {
        /* ★★ 必须 LV_ANIM_OFF, 不能用 LV_ANIM_ON ——
         *   lv_bar_set_value(bar, v, LV_ANIM_ON) 每次都会 lv_anim_start **新加**一条动画,
         *   而 LVGL 不会替换同对象上的旧动画 ✗
         *   网关每 2 秒推一次 × 3 个进度条 → 每分钟堆 90 条;
         *   停在 Server 屏十几分钟就能攒上千条 → lv_timer_handler 每轮都要遍历整个
         *   动画链表 → 界面越来越卡, 最后整页卡死 ✗
         *   (实测踩过: 停在 Server 屏久了自己卡住, 转走再回来恢复正常)
         *   ⇒ 进度条这种"每两秒刷一次"的东西不需要过渡动画, 直接跳值 ✓ */
        lv_bar_set_value(bar, (int32_t)pct, LV_ANIM_OFF);
    }
}

bool is_active(void)
{
    return (ui_Server != nullptr) && (lv_screen_active() == ui_Server);
}

void apply(const usage_t *u)
{
    if (u == nullptr) {
        return;
    }

    /* 各组的"上次写入值" —— 只在变化时才真的动控件。
     * ★ 网关每 2 秒推一次, 而 CPU/内存往往几秒才变一点 → 大部分轮次是空转 ✓ */
    static int16_t c_cpu = -1, c_ram = -1, c_disk = -1, c_rx = -1, c_tx = -1;
    static char    c_ram_used[16]  = "\x01";   /* 用一个不可能等于真实数据的初值 */
    static char    c_disk_used[16] = "\x01";
    static char    c_cpu_str[16]   = "\x01";

    /* ---- CPU ---- */
    set_pct(ui_cpunum, ui_cpubar, u->cpu, &c_cpu);

    /* ---- RAM ---- */
    set_pct(ui_ramnum, ui_rambar, u->ram_pct, &c_ram);
    if (strcmp(c_ram_used, u->ram_used) != 0) {
        strlcpy(c_ram_used, u->ram_used, sizeof(c_ram_used));
        if (ok(ui_ramtext)) {
            lv_label_set_text(ui_ramtext, u->ram_used);
        }
    }

    /* ---- 磁盘 ---- */
    set_pct(ui_disknum, ui_diskbar, u->disk_pct, &c_disk);
    if (strcmp(c_disk_used, u->disk_used) != 0) {
        strlcpy(c_disk_used, u->disk_used, sizeof(c_disk_used));
        if (ok(ui_dixktext)) {
            lv_label_set_text(ui_dixktext, u->disk_used);
        }
    }

    /* ---- 上下行速度 ----
     * ★ api 的 rx = 下载, tx = 上传。
     *   屏幕上 ui_downnum 配的是 ⬇ 图标 (下载), ui_upnum 配的是 ⬆ (上传) → rx 给 down ✓ */
    if (c_rx != u->rx) {
        c_rx = u->rx;
        set_speed(ui_downnum, u->rx);      /* ★ kb/mb 自动换单位, 免得撑破文本框 */
    }
    if (c_tx != u->tx) {
        c_tx = u->tx;
        set_speed(ui_upnum, u->tx);
    }

    /* ---- CPU 负载文字 (ui_cputext, SquareLine 里是 "0.3/2 Core") ----
     * ★ 网关字段 cpu_str, 原样显示 —— 空串时保留 SquareLine 的静态文字。 */
    if (u->cpu_str[0] != '\0' && strcmp(c_cpu_str, u->cpu_str) != 0) {
        strlcpy(c_cpu_str, u->cpu_str, sizeof(c_cpu_str));
        if (ok(ui_cputext)) {
            lv_label_set_text(ui_cputext, u->cpu_str);
        }
    }

    /* ---- 网络速度曲线 (ui_rxtxchar, LV_CHART_TYPE_LINE, 5 个点) ----
     * ★★ 每条数据都 push 一个点, 并【动态改 Y 轴量程】让两条曲线都落在图里。
     *   量程取 max(rx, tx) 向上取整到"好读的档位" (5/10/20/50/100...) ——
     *   固定 0~15 的话, 一旦速度上百曲线就全贴顶了 ✗ */
    if (ok(ui_rxtxchar) && lv_obj_check_type(ui_rxtxchar, &lv_chart_class)) {
        /* ★★ SquareLine 里【已经建好两条 series 了】(ui_Server.c:339-346):
         *      series_1 = 0x62A5FF 蓝   → 下行 (rx)
         *      series_2 = 0xF94848 红   → 上行 (tx)
         *   ⚠️ 它们都是 ui_Server_screen_init() 的局部变量 → 只能靠 API 顺序取:
         *      lv_chart_get_series_next(chart, NULL)        → 第 1 条
         *      lv_chart_get_series_next(chart, 第 1 条)      → 第 2 条
         *   ❌ 之前我在这里 lv_chart_add_series(黄色) 是错的: 图里会多出一条用不到的
         *      黄线, 而且 tx 被画成黄色而不是设计里的红色 ✗ */
        static lv_chart_series_t *s_rx = nullptr;
        static lv_chart_series_t *s_tx = nullptr;
        if (s_rx == nullptr) {
            s_rx = lv_chart_get_series_next(ui_rxtxchar, nullptr);
            if (s_rx != nullptr) {
                s_tx = lv_chart_get_series_next(ui_rxtxchar, s_rx);
            }
            ESP_LOGI(TAG, "曲线 series: 蓝(rx)=%p 红(tx)=%p", (void *)s_rx, (void *)s_tx);
        }
        if (s_rx != nullptr) {
            lv_chart_set_next_value(ui_rxtxchar, s_rx, u->rx);   /* 蓝 = 下行 */
        }
        if (s_tx != nullptr) {
            lv_chart_set_next_value(ui_rxtxchar, s_tx, u->tx);   /* 红 = 上行 ✓ */
        }

        /* 动态 Y 量程: 取两条里大的, 再留 20% 余量并向上取到档位 */
        int32_t peak = (u->rx > u->tx) ? u->rx : u->tx;
        int32_t top;
        if      (peak <= 5)   top = 5;
        else if (peak <= 10)  top = 10;
        else if (peak <= 20)  top = 20;
        else if (peak <= 50)  top = 50;
        else if (peak <= 100) top = 100;
        else                  top = ((peak / 100) + 1) * 100;
        static int32_t s_top = -1;
        if (top != s_top) {
            s_top = top;
            lv_chart_set_axis_range(ui_rxtxchar, LV_CHART_AXIS_PRIMARY_Y, 0, top);
        }
    }
}

}  // namespace server_view
