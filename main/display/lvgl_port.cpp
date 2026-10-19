#include "display/lvgl_port.hpp"
#include "display/lcd_display.hpp"
#include "drivers/encoder.hpp"

#include <stdint.h>
#include <string.h>          /* strcmp / strlcpy —— 时间上屏用 */
#include <stdio.h>           /* snprintf */
#include <time.h>            /* time / localtime_r —— 时间上屏 + 启动门判断 NTP */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"        /* esp_wifi_sta_get_ap_info —— 启动门判断 WiFi */
#include "driver/spi_master.h"

#include "drivers/ws.hpp"    /* WS::get().is_connected() —— 启动门判断网关握手 */
#include "drivers/weather.hpp" /* weather::has_data() —— 启动门判断天气是否已到 */
#include "lvgl.h"
#include "ui.h"              /* ui_time3 / ui_time4 / ui_home / ui_loadingtext */

static const char *TAG = "lvgl";

/* ------------------ 参数 ------------------ */
#define LVGL_SPI_HOST       (SPI2_HOST)
#define LVGL_TICK_PERIOD_MS (2)
/* 绘图缓冲的两种模式:
 *
 *   FULL=1  双整屏 —— 每帧都把【整屏】重画+传出去, 哪怕只有一个字变了。
 *           实测 lv_timer_handler() 约 137ms(≈7FPS)。
 *   FULL=0  partial —— **只重画/只传【脏区】**。这是我们要的。
 *
 * ★ 40MHz 的 SPI 传整屏 106KB 要 21.7ms, 所以 FULL 模式光是传输就吃掉大半预算;
 *   partial 只传脏区, 传输量按脏区面积缩小。
 *
 * ★★ partial 的 band 高度为什么取【整屏高】而不是小块 (LVGL_PARTIAL_LINES = V_RES):
 *
 *   partial 是按 band 逐条 flush 的。band 取小了 (比如 40 行) 会有【果冻】:
 *     band1 发出后动画已经推进 → band1 是帧 N, band5 可能已是帧 N+2
 *     → 屏幕上同时存在几个时刻的画面 ✗ (转屏动画时整屏都是脏区, 5 个 band, 最明显)
 *
 *   取到整屏高之后:
 *     · 脏区够大时 → 一次装下 → **一次 flush = 一个快照** → 果冻消失 ✓
 *     · 脏区小时   → **仍然只渲染那几百像素** (不像 FULL 要整屏重来) → 提速保留 ✓
 *
 *   ⚠️ 代价: 缓冲回到 320*170*2 = 106KB x2 = 212KB (PSRAM, 8MB 富余, 不是问题)。
 *   ⚠️ 若仍见撕裂/果冻, 就把 LVGL_RENDER_FULL 调回 1 退回双整屏。 */
#define LVGL_RENDER_FULL    (0)
#define LVGL_BUF_IN_PSRAM   (1)         /*!< 1=绘图缓冲放 PSRAM (SPI2+GDMA 直读, 自动 cache sync);
                                            0=内部 DMA SRAM。
                                            ★ 放内部 SRAM 渲染更快 (LVGL 每像素写 PSRAM 比写 SRAM 慢),
                                              但缓冲有 106KB, 内部 SRAM 装不下 → **别改成 0**。 */
#define LVGL_TASK_PRIO      (2)
#define LVGL_TASK_STACK     (6 * 1024)
#define LVGL_TASK_CORE      (1)         /*!< CPU1 (网络 ws_task 在 CPU0) */

static SemaphoreHandle_t s_lvgl_lock = NULL;

/* flush 完成回调: SPI DMA 把一帧传完后由 panel IO 事件触发 */
static bool lvgl_flush_ready_cb(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)io;
    (void)edata;
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    return false;   /* 不截获, 继续后续 */
}

/* LVGL flush 回调: 把渲染好的区域经 SPI 刷到面板 */
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;

    /* SPI LCD 期望大端字节序, RGB565 需交换两个字节; 若颜色仍乱, 改 LCD_SWAP_BYTES=0 试 */
#if LCD_SWAP_BYTES
    lv_draw_sw_rgb565_swap(px_map, (uint32_t)(w * h));
#endif
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}

/* 2ms tick: 喂给 LVGL 时间基准 */
static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

/* LVGL 主循环任务 (CPU1) */
static void lvgl_port_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "LVGL task started");
    while (1) {
        lvgl_port_lock();
        uint32_t wait_ms = lv_timer_handler();
        lvgl_port_unlock();

        /* 先按 ms 夹紧上界: wait_ms 可能是 LV_NO_TIMER_READY(0xFFFFFFFF),
         * 直接进 pdMS_TO_TICKS 会溢出。 */
        if (wait_ms > 500) {
            wait_ms = 500;    /* 避免长时间不调度 */
        }

        /* ★★ 必须保证换算后【至少 1 个 tick】, 不能只按 ms 夹紧:
         *   HZ=100 时 pdMS_TO_TICKS(1..9)==0, 而 vTaskDelay(0) **不让出 CPU**
         *   → 渲染(实测~137ms) > LVGL 周期(33ms) 时 lv_timer_handler 总返回极小值
         *   → 本任务占满 CPU1 → IDLE1 永不调度 → 5s 后 Task WDT。
         *   详见 MD/HANDOFF.md §6 (IDF/FreeRTOS 表) */
        TickType_t ticks = pdMS_TO_TICKS(wait_ms);
        if (ticks < 1) {
            ticks = 1;
        }
        vTaskDelay(ticks);
    }
}

esp_err_t lvgl_port_init(void)
{
    /* 1. LCD 硬件 */
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    esp_err_t ret = lcd_panel_init(&panel, &io);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 2. LVGL 锁 */
    s_lvgl_lock = xSemaphoreCreateMutex();
    if (s_lvgl_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 3. lv_init + display */
    lv_init();
    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    if (disp == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 4. 绘图缓冲: FULL=双整屏 320*170*2 ≈106KB x2; PSRAM=1 时放 PSRAM
     *    (SPI2 + GDMA 直读, 驱动自动 cache sync/对齐)
     *    ★ partial 的 band 高度取【整屏高】—— 理由见文件上方 LVGL_RENDER_FULL 那段注释
     *      (取小了会在转屏动画时出现"果冻": band 之间动画已推进, 屏幕上拼了不同帧)。 */
#if LVGL_RENDER_FULL
    size_t buf_sz = LCD_H_RES * LCD_V_RES * sizeof(lv_color16_t);
    lv_display_render_mode_t render_mode = LV_DISPLAY_RENDER_MODE_FULL;
#else
    #define LVGL_PARTIAL_LINES (LCD_V_RES)   /*!< = 170, 一次装下整个脏区 */
    size_t buf_sz = LCD_H_RES * LVGL_PARTIAL_LINES * sizeof(lv_color16_t);
    lv_display_render_mode_t render_mode = LV_DISPLAY_RENDER_MODE_PARTIAL;
#endif
#if LVGL_BUF_IN_PSRAM
    uint32_t buf_caps = MALLOC_CAP_SPIRAM;
#else
    uint32_t buf_caps = MALLOC_CAP_INTERNAL;
#endif
    void *buf1 = spi_bus_dma_memory_alloc(LVGL_SPI_HOST, buf_sz, buf_caps);
    void *buf2 = spi_bus_dma_memory_alloc(LVGL_SPI_HOST, buf_sz, buf_caps);
    if (buf1 == NULL || buf2 == NULL) {
        ESP_LOGE(TAG, "draw buffer 分配失败 (%u B x2)", (unsigned)buf_sz);
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_buffers(disp, buf1, buf2, buf_sz, render_mode);
    lv_display_set_user_data(disp, panel);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);

    /* 5. 注册 flush 完成事件 → lv_display_flush_ready */
    esp_lcd_panel_io_callbacks_t cbs = {};
    cbs.on_color_trans_done = lvgl_flush_ready_cb;
    esp_lcd_panel_io_register_event_callbacks(io, &cbs, disp);

    /* 6. tick 定时器 */
    esp_timer_create_args_t tick_args = {};
    tick_args.callback = lvgl_tick_cb;
    tick_args.arg = NULL;
    tick_args.dispatch_method = ESP_TIMER_TASK;
    tick_args.name = "lvgl_tick";
    tick_args.skip_unhandled_events = false;
    esp_timer_handle_t tick_timer = NULL;
    ret = esp_timer_create(&tick_args, &tick_timer);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 7. LVGL 任务: CPU1, priority 2 */
    xTaskCreatePinnedToCore(lvgl_port_task, "lvgl", LVGL_TASK_STACK, NULL,
                            LVGL_TASK_PRIO, NULL, LVGL_TASK_CORE);

#if LVGL_RENDER_FULL
    ESP_LOGI(TAG, "LVGL 9.5 port ready: %dx%d, FULL 双整屏 ~%uKB x2 (%s), pclk=%dHz",
             LCD_H_RES, LCD_V_RES, (unsigned)(buf_sz / 1024),
             (buf_caps & MALLOC_CAP_SPIRAM) ? "PSRAM" : "SRAM", LCD_PIXEL_CLOCK_HZ);
#else
    ESP_LOGI(TAG, "LVGL 9.5 port ready: %dx%d, partial %d行 (%s), pclk=%dHz",
             LCD_H_RES, LCD_V_RES, LVGL_PARTIAL_LINES,
             (buf_caps & MALLOC_CAP_SPIRAM) ? "PSRAM" : "SRAM", LCD_PIXEL_CLOCK_HZ);
#endif
    return ESP_OK;
}

void lvgl_port_lock(void)
{
    if (s_lvgl_lock != NULL) {
        xSemaphoreTake(s_lvgl_lock, portMAX_DELAY);
    }
}

void lvgl_port_unlock(void)
{
    if (s_lvgl_lock != NULL) {
        xSemaphoreGive(s_lvgl_lock);
    }
}

/* ============ "选择模型"面板的焦点组 ============
 * ★★ 为什么要单独一套: 平常编码器所在的默认组里【只有整屏】, 而且被强制编辑模式
 *    (editing=true) —— 旋转会给屏发 LV_KEY_LEFT/RIGHT, SquareLine 靠它切屏。
 *    面板要的是反过来的: 组里两个图标 + 导航模式 (旋转 = 移动焦点, 不发键)。
 *    ⇒ 所以给面板单开一个组, 进出时把 indev【换绑】过去 / 换回来。
 * ⚠️ 面板模式期间 lvgl_focus_active_screen_locked() 必须短路 (见下面的 s_cho_mode)。 */
static lv_indev_t *s_enc_indev    = nullptr;   /*!< 编码器 indev, 换绑要用 */
static lv_group_t *s_screen_group = nullptr;   /*!< 默认组: 只有整屏, 编辑模式 → 切屏 */
static lv_group_t *s_cho_group    = nullptr;   /*!< 面板组: 两个图标, 导航模式 → 移动焦点 */
static bool        s_cho_mode     = false;     /*!< true = 面板开着, 独占编码器 */
static lv_obj_t   *s_focus_override = nullptr; /*!< ★ 当前屏指定拿焦点的对象 (NULL=整屏) */

/* 确保"当前激活屏幕"是 group 里唯一且被聚焦的对象 (无锁, 须在持锁上下文调用)。
 * [链] 【本函数】 ← lvgl_encoder_read_cb (每轮) → 组内只留当前屏 + 重申编辑模式
 *      → 聚焦屏才收得到 LV_EVENT_KEY → SquareLine 切屏回调
 *
 * ★★ 必须重申【编辑模式】: lv_group_focus_obj() 内部第一件事就是
 *    lv_group_set_editing(g, false) (lv_group.c:242) —— 只要它被调用, 编辑模式就没了;
 *    没了之后旋转退化成 navigate(只 focus_next/prev, 不发 LV_KEY_LEFT/RIGHT)
 *    → 屏幕完全无反应。旧屏残留还会让"每次都调 focus_obj", 每轮清一次。
 *    详见 MD/HANDOFF.md §5.1 */
static void lvgl_focus_active_screen_locked(lv_group_t *g)
{
    /* ★★ 面板模式短路 —— 这是整套"面板焦点"能成立的前提。
     *   面板期间 indev 已经换绑到 s_cho_group, 这里如果继续跑, 会:
     *     ① 把【默认组】清成只剩整屏 (无害, 反正没用)
     *     ② 把默认组强制 editing=true (也无害)
     *   但真正致命的是下面这段每 30ms 执行一次的行为会让人误以为焦点该在屏上 ——
     *   所以直接早退, 语义清楚。 */
    if (s_cho_mode) {
        return;
    }
    if (g == NULL) {
        return;
    }
    lv_obj_t *scr = lv_screen_active();
    if (scr == NULL) {
        return;
    }

    /* ★★ 焦点对象: 默认是整屏 (SquareLine 的屏级 LV_EVENT_KEY 负责切屏),
     *   但业务层可以指定"当前屏上别的对象来拿焦点" (聊天屏的 ui_contextPanel 就是)。
     *   那个对象自己消费 LV_KEY_LEFT/RIGHT 做滚动, 滚到头才放行冒泡到屏 ✓
     *   ⚠️ override 必须还在当前屏里 —— 否则换了屏它就成了孤儿, 焦点会飘到看不见的对象上。 */
    lv_obj_t *target = scr;
    if (s_focus_override != nullptr && lv_obj_is_valid(s_focus_override)
        && lv_obj_get_screen(s_focus_override) == scr) {
        target = s_focus_override;
    }

    /* 组里只留 target。稳定态下两个条件都成立 → 整块跳过, 无额外开销/无重绘。 */
    if (lv_obj_get_group(target) != g || lv_group_get_focused(g) != target) {
        lv_group_remove_all_objs(g);        /* 旧对象全部移出 (否则触发上面坑 2) */
        lv_group_add_obj(g, target);
        if (lv_group_get_focused(g) != target) {
            lv_group_focus_obj(target);
        }
    }

    /* 编辑模式: 旋转必须发 LV_KEY_LEFT/RIGHT (SquareLine 监听的是 LV_EVENT_KEY)。
     * 放在最后 + 幂等 (内部 editing 相同则直接 return) → 自愈上面两个坑。 */
    if (!lv_group_get_editing(g)) {
        lv_group_set_editing(g, true);
    }
}

/* ============ 编码器 indev (无任务, 由 lv_timer_handler 周期驱动) ============
 * [链] GPIO中断(A/B双相) → encoder_isr → s_accum
 *      → 【lvgl_encoder_read_cb】 → data->enc_diff → LVGL → LV_KEY_LEFT/RIGHT
 *      → 聚焦对象 → 冒泡到屏 → SquareLine 切屏回调
 * 本回调在 lv_timer_handler 内部被调, 已持 LVGL 锁 → 禁止再加锁。 */

static int s_enc_carry = 0;    /*!< 不足一格的跳变余数 (跨帧累积) */

/*! 编码器输入开关 —— 录音期间禁止转屏 (写它的是 voice_task/CPU0, 读它的是 lvgl 任务/CPU1) */
static volatile bool s_enc_enabled = true;
void lvgl_port_set_encoder_enabled(bool enabled)
{
    s_enc_enabled = enabled;
    ESP_LOGI(TAG, "编码器输入 %s", enabled ? "已恢复" : "已暂停 (录音中, 禁止转屏)");
}

static void lvgl_encoder_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    int32_t raw = encoder_consume_raw();      /* ★ 无论开关状态都必须先取走 */

    if (!s_enc_enabled) {
        /* ★★ 暂停时【故意丢弃】raw 和已有余数: 攒着的话, 恢复那一刻会一次性倾泻
         *    几十格 → 连跳好几屏。语义是"录音期间转的不算数"。 */
        s_enc_carry    = 0;
        data->enc_diff = 0;
        data->key      = LV_KEY_ENTER;
        data->state    = LV_INDEV_STATE_RELEASED;
        /* 聚焦逻辑仍要跑(幂等), 万一期间屏幕因别的原因变了, 焦点要跟上 */
        lv_group_t *g = lv_group_get_default();
        if (g != NULL) {
            lvgl_focus_active_screen_locked(g);
        }
        return;
    }

    s_enc_carry += raw;

    int32_t steps = s_enc_carry / ENC_KEY_STEP;   /* 整格数 (C 语言负数除法向零截断) */
    s_enc_carry -= steps * ENC_KEY_STEP;          /* 余数留到下轮 */

    data->enc_diff = (int16_t)steps;              /* 正=右, 负=左 */
    data->key      = LV_KEY_ENTER;                /* 编码器无按键, 占位 */
    data->state    = LV_INDEV_STATE_RELEASED;     /* 永远松开 (无按下源) */

    /* 每轮确保聚焦当前屏: 切屏后新屏才能收到后续旋转键 */
    lv_group_t *g = lv_group_get_default();
    if (g != NULL) {
        lvgl_focus_active_screen_locked(g);
    }
}

esp_err_t lvgl_port_register_encoder_indev(void)
{
    /* 1. 默认 group (SquareLine 导出不建组) */
    lv_group_t *g = lv_group_get_default();
    if (g == NULL) {
        g = lv_group_create();
        lv_group_set_default(g);
    }
    s_screen_group = g;              /* ★ 存起来: 面板进出时要换绑回它 */

    /* 2. ★ 编辑模式: 旋转必须发 LV_KEY_LEFT/RIGHT 给聚焦对象 (SquareLine 监听的是前者),
     *    而不是移动焦点(导航模式)。见 lvgl_focus_active_screen_locked() 的坑。 */
    lv_group_set_editing(g, true);

    /* 3. 建 indev 并绑定 */
    lv_indev_t *indev = lv_indev_create();
    if (indev == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_indev_set_type(indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(indev, lvgl_encoder_read_cb);
    lv_indev_set_group(indev, g);
    s_enc_indev = indev;             /* ★ 存起来: 面板进出时要换绑 */

    lvgl_focus_active_screen_locked(g);

    ESP_LOGI(TAG, "encoder indev 注册完成 (编辑模式, 发 LV_KEY_LEFT/RIGHT)");
    return ESP_OK;
}

/* ================================================================
 * "选择模型"面板的焦点接管
 * ================================================================ */

void lvgl_port_cho_enter(void *a, void *b, void *focus_obj)
{
    /* ① 组是【懒建】的: 第一次被调时 ui_init() 早跑完了, 控件都存在。
     *    (不能放在 lvgl_port_register_encoder_indev 里建 —— 那比 ui_init 早, 控件还是 NULL。) */
    if (s_cho_group == nullptr) {
        s_cho_group = lv_group_create();
        if (s_cho_group == nullptr) {
            ESP_LOGE(TAG, "聚光灯焦点组创建失败");
            return;
        }
        /* ★★ 导航模式 (editing=false): 旋转 → focus_next/prev, 【不发】LV_KEY_LEFT/RIGHT。
         *    这样面板里转编码器就【不会冒泡到屏】触发 SquareLine 的切屏回调 ✓ */
        lv_group_set_editing(s_cho_group, false);
    }

    /* ② 每次 enter 都重建组内容 → 模型面板和通知面板可以共用这一个组, 不会串对象。 */
    lv_group_remove_all_objs(s_cho_group);
    lv_obj_t *items[2] = {(lv_obj_t *)a, (lv_obj_t *)b};
    for (int i = 0; i < 2; i++) {
        if (items[i] != nullptr && lv_obj_is_valid(items[i])) {
            /* 让对象可聚焦 + 可点 (LVGL 的焦点/点击判定都看这个 flag) */
            lv_obj_add_flag(items[i], LV_OBJ_FLAG_CLICKABLE);
            lv_group_add_obj(s_cho_group, items[i]);
        }
    }

    s_cho_mode = true;
    /* ③ 把编码器【换绑】到聚光灯组 */
    if (s_enc_indev != nullptr) {
        lv_indev_set_group(s_enc_indev, s_cho_group);
    }

    /* ④ 焦点落到指定项。★ 必须显式 focus —— 换绑本身不会移动焦点。 */
    lv_obj_t *want = (lv_obj_t *)focus_obj;
    if (want == nullptr || !lv_obj_is_valid(want)) {
        want = items[0];
    }
    if (want != nullptr && lv_obj_is_valid(want)) {
        lv_group_focus_obj(want);
    }

    ESP_LOGI(TAG, "聚光灯: 接管编码器 (导航模式)");
}

void lvgl_port_cho_exit(void)
{
    if (!s_cho_mode) {
        return;
    }
    s_cho_mode = false;

    /* 换回屏幕组。★ 下一轮 lvgl_encoder_read_cb 里的 lvgl_focus_active_screen_locked()
     * 会把组重置成"当前屏 + 编辑模式", 所以这里只需要把 indev 绑回去。 */
    if (s_enc_indev != nullptr && s_screen_group != nullptr) {
        lv_indev_set_group(s_enc_indev, s_screen_group);
    }
    ESP_LOGI(TAG, "面板焦点: 交还编码器 (恢复切屏)");
}

void *lvgl_port_cho_focused(void)
{
    if (!s_cho_mode || s_cho_group == nullptr) {
        return nullptr;
    }
    return (void *)lv_group_get_focused(s_cho_group);
}

void lvgl_port_set_focus_override(void *obj)
{
    /* ★ 只存指针, 不动 group —— 真正的切换由下一轮 lvgl_focus_active_screen_locked
     *   统一做 (它每 ~33ms 跑一次)。这里做的话会出现"两处都在改 group"的竞争。 */
    s_focus_override = (lv_obj_t *)obj;
}

/* ================================================================
 * 时间上屏 —— ui_time3(时:分) / ui_time4(:秒)
 *
 * ★ 为什么放在 lvgl_port 而不是新开任务:
 *   lv_timer 的回路由 lv_timer_handler() 调用, 那时 **LVGL 锁已经持有** →
 *   直接改控件即可, 不用 lvgl_port_lock, 也不会和渲染抢锁。
 *   新开一个任务每秒 lvgl_port_lock() 反而要多一份栈, 而且锁是 portMAX_DELAY
 *   的阻塞取, 万一 LVGL 侧卡住那个任务就永久挂死了。
 *
 * ★ 为什么不用"自己的秒计数器累加":
 *   lv_timer 名义 1000ms, 但 lv_timer_handler 每 ~137ms 才跑一轮 → 实际触发有抖动,
 *   累加会慢慢漂移、偶尔跳秒。**每次读 RTC 就不会** —— 时间是权威的, 我们只负责显示。
 *
 * ★ 时间从哪来: SNTP 在 WiFi 拿到 IP 后自动对时 (drivers/wifi.cpp 的 sntp_start_once)。
 *   没同步完之前 tm_year 是 70 (1970), 这里直接清空 label, 免得显示假时间。
 * ================================================================ */
#define TIME_TIMER_PERIOD_MS  (1000)

/*! 把 RTC 的时分秒写到 ui_time3 / ui_time4。只在 LVGL 任务里跑 (lv_timer 回调)。 */
static void time_refresh_locked(void)
{
    time_t   now = 0;
    struct tm tm_now = {};
    time(&now);
    localtime_r(&now, &tm_now);

    /* ★ 未同步: 年份 < 2020 一律当"还没对时"。清空而不是显示 1970 的假时间。
     *   ⚠️ 不要写 "--:--": ui_font_time52/time25 里只有 0123456789: , 没有 '-' 这个字形,
     *      写上去只会显示成空白, 不如干脆清空。 */
    if (tm_now.tm_year < 120) {
        if (ui_time3 != NULL && lv_obj_is_valid(ui_time3)) {
            lv_label_set_text(ui_time3, "");
        }
        if (ui_time4 != NULL && lv_obj_is_valid(ui_time4)) {
            lv_label_set_text(ui_time4, "");
        }
        return;
    }

    char buf[8];

    /* ---- 12 小时制: 0 点显示 12, 13 点显示 1 ----
     * 0 % 12 = 0 → 要显示 12 (凌晨 12:xx); 12 % 12 = 0 → 也显示 12 (中午 12:xx);
     * 13 % 12 = 1 … 23 % 12 = 11 ✓ 所以"为 0 就换成 12"一条就够。
     * ⚠️ 没有 AM/PM 标记 —— ui_font_time52 里只有 "0123456789:" 这些字形, 没有 A/P/M 字母,
     *    写 "AM" 只会显示成空白。要加标记得给标签换字体 (montserrat) 或者另开一个 label。 */
    int h12 = tm_now.tm_hour % 12;
    if (h12 == 0) {
        h12 = 12;
    }

    /* ---- ui_time3: "H:MM" —— 每分钟才变, 值没变就别写 ----
     * ★ 小时【不补零】(7 点显示 "7", 不是 "07"), 分钟仍然补零 ("7:05") —— 12 小时制的惯例。
     *
     * ⚠️ 代价: 小时不补零 → 字符串长度会在 4/5 字符之间变 (7:05 ↔ 12:05)。
     *    ui_time3 宽度是 LV_SIZE_CONTENT, 而它和 ui_time4(:秒) 在 ui_timepanel 的 flex row 里,
     *    → **秒数会跟着横移约一个字的宽度** (一天里只有几次: 9→10 点、12→1 点)。
     *    想彻底消除这个位移: 在 SquareLine 里把 ui_time3 的宽度改成固定值 (+ text_align = RIGHT),
     *    这样它宽度恒定, 秒数就永远不动了。
     *
     * 不比较的话: timer 每秒触发, 但每分钟只有一次真的变 → 其余 59 次是白 malloc + 白重绘。 */
    static char last_hm[8] = "";
    snprintf(buf, sizeof(buf), "%d:%02d", h12, tm_now.tm_min);
    if (strcmp(buf, last_hm) != 0) {
        strlcpy(last_hm, buf, sizeof(last_hm));
        if (ui_time3 != NULL && lv_obj_is_valid(ui_time3)) {
            lv_label_set_text(ui_time3, buf);
        }
    }

    /* ---- ui_time4: ":SS" —— 每秒必变, 直接写 ---- */
    snprintf(buf, sizeof(buf), ":%02d", tm_now.tm_sec);
    if (ui_time4 != NULL && lv_obj_is_valid(ui_time4)) {
        lv_label_set_text(ui_time4, buf);
    }

    /* ---- ui_date1: "12月30日\n星期四" —— 每天才变, 值没变就别写 ----
     *
     * ★ 星期直接用 tm_wday 查表, 不用自己算:
     *     tm_wday: 0=周日 1=周一 … 6=周六   (localtime_r 已经换算好了)
     *   ⚠️ 中文习惯是"星期一"而不是"周几" —— 而且 ui_font_date18 里【没有"周"这个字】,
     *      只有 年月日星期一二三四五六 → 所以只有"星期X"这一种写法能显示出来。
     *
     * ★ tm_mon 是 0~11 (0=一月) → 显示要 +1; tm_mday 是 1~31 直接用。
     *
     * ⚠️ 缓冲区按 UTF-8 字节数算: "12月31日\n星期六" = 2+3+2+3+1+3+3+3 = 20 字节,
     *    再加 NUL → 32 字节绰绰有余 (buf[8] 那个是给时间用的, 装不下这个)。 */
    static const char *const kWeekdayCN[] = { "日", "一", "二", "三", "四", "五", "六" };
    static char last_date[32] = "";
    char dbuf[32];
    snprintf(dbuf, sizeof(dbuf), "%d月%d日\n星期%s",
             tm_now.tm_mon + 1, tm_now.tm_mday, kWeekdayCN[tm_now.tm_wday % 7]);
    if (strcmp(dbuf, last_date) != 0) {
        strlcpy(last_date, dbuf, sizeof(last_date));
        if (ui_date1 != NULL && lv_obj_is_valid(ui_date1)) {
            lv_label_set_text(ui_date1, dbuf);
        }
    }
}

static void time_timer_cb(lv_timer_t *t)
{
    (void)t;
    time_refresh_locked();
}

esp_err_t lvgl_port_time_start(void)
{
    /* ★ 先立刻刷一次, 不然要等到第一个 1000ms 才有内容 */
    time_refresh_locked();

    if (lv_timer_create(time_timer_cb, TIME_TIMER_PERIOD_MS, NULL) == NULL) {
        ESP_LOGE(TAG, "时间刷新 timer 创建失败");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "时间上屏已启动 (每 %dms → ui_time3 / ui_time4)", TIME_TIMER_PERIOD_MS);
    return ESP_OK;
}

/* ================================================================
 * 开屏门 —— 三项配置就绪 + 动画播完 后离开开屏屏
 *
 * 原来的行为: ui_main.c 的 LV_EVENT_SCREEN_LOADED 里排了一次
 *     _ui_screen_change(&ui_home, FADE_ON, 500, 7000, …)
 * 也就是【死等 7 秒】, 不管有没有连上。
 *
 * 现在: 每 200ms 查一次 WiFi / 网关握手 / NTP。
 *   **等满 BOOT_GATE_MIN_WAIT_MS (4 秒, 让开屏动画播完)** 并且三项齐了, 才切。
 *   动画没播完 → 继续等; 配置没齐 → 继续等; 两者都满足 → 立刻切。
 *
 * ★★★ 前提: ui_main.c 里那句 _ui_screen_change 已经被【删掉】(见那份文件的注释)。
 *   为什么必须删, 而不是在这里撤销它:
 *     lv_screen_load_anim() 一进去就设 d->scr_to_load = ui_home, 而这个字段只有
 *     动画的 start_cb(load_new_screen) / completed_cb(scr_anim_completed) 会清回 NULL。
 *     只要那个延迟加载还悬着, 我们这里的 _ui_screen_change(&ui_home, …) 就会被
 *     lv_screen_load_anim 开头那句
 *         if(act_scr == new_scr || d->scr_to_load == new_scr) return;
 *     静默吞掉 → 现象是【日志说"切主界面"但屏幕纹丝不动, 永久卡在开屏】。
 *     想提前撤销它只能用 lv_anim_delete(ui_home, NULL), 但那会让两个回调都不跑、
 *     scr_to_load 永久悬空 —— 更糟 (而且实测会连着踩坏 LVGL 内部状态, 偶发 LoadProhibited)。
 *   ⇒ 结论: 延迟加载压根不该排, 换屏由本模块独占。
 *
 * ★ 兜底: 等满 BOOT_GATE_MAX_WAIT_MS 还没齐就强制切 —— 否则 WiFi 配置错的机器
 *   会永远停在开屏屏, 连"哪一步没成"都看不到。
 * ================================================================ */
#define BOOT_GATE_PERIOD_MS    (200)
/*! ★ 最小等待: 开屏动画本身要播完, 提前切会把它掐断。
 *  ui_main.c 里的动画: loading 2000ms + link 500ms + Lan 500ms + loadingtext 2000ms,
 *  取 4 秒盖住它们。这段时间内【不管四项是否就绪都不切】。 */
#define BOOT_GATE_MIN_WAIT_MS  (4000)
/*! 兜底上限: 等满这么久还没就绪就强制切 —— 否则 WiFi 配置错的机器会永远停在开屏屏。 */
#define BOOT_GATE_MAX_WAIT_MS  (20000)
/*! ★ 天气单独一条【软上限】: 其余三项都好了之后, 最多再为天气多等这么久。
 *
 *  ★ 为什么单独设一条而不是跟着 20 秒走:
 *    天气要靠 weather_poll 切 svc=weather 才拿得到 (见 tools/gw_probe_weather.mjs
 *    实测结论: 其他服务下发 {"type":"get"} 会被网关 echo 吞掉)。
 *    而 weather_poll 首次是启动后 3 秒才发, 拿到约 4~5 秒 —— 此时下面这个 10 秒
 *    窗口足够; 但万一网关没推 / 轮询恰好在"忙"里被推迟 1 分钟, 那就不该再等,
 *    否则开屏会白白卡到 20 秒 ✗
 *    → 超时后照样切主界面, 天气到了再在后台更新 (weather_view 会自己刷新)。 */
#define BOOT_GATE_WEATHER_WAIT_MS  (10000)

static uint32_t boot_gate_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/*! WiFi 是否已连上 —— 用 IDF 接口查, 不去依赖 main.cpp 里那个 static WiFi 实例。 */
static bool boot_gate_wifi_ok(void)
{
    wifi_ap_record_t ap = {};
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

/*! 网关握手是否完成 (WS 已连上)。 */
static bool boot_gate_ws_ok(void)
{
    return WS::get().is_connected();
}

/*! NTP 是否已同步 —— esp_netif_sntp 没有"是否已同步"的查询接口,
 *  所以直接看系统时间: 2023-11 之后的时间戳 = 已经对过时了。 */
static bool boot_gate_ntp_ok(void)
{
    return time(NULL) > 1700000000;      /* 2023-11-14 之后 */
}

/*! 天气是否已到。
 *  ★ 为什么要等它: 主界面在收到第一条 weather 之前显示的是 SquareLine 的占位物
 *    (温度 "18"、文字 "小雨"、图 cloudy) —— 等天气到了再切屏, 用户看不到假数据。
 *  ⚠️ 这是【软条件】: 只多等 BOOT_GATE_WEATHER_WAIT_MS, 超时就不等了。
 *    理由: 网关可能根本没推 (网络/服务异常), 或者天气轮询恰好在"忙"里被推迟
 *    1 分钟 —— 那种情况绝不该把开屏卡到 20 秒上限。 */
static bool boot_gate_weather_ok(void)
{
    return weather::has_data();
}

static void boot_gate_cb(lv_timer_t *t)
{
    static uint32_t t0 = 0;
    if (t0 == 0) {
        t0 = boot_gate_now_ms();
    }

    /* 已经切到主界面了 → 收工 (换屏由本模块独占, 不会有别人抢先切) */
    if (lv_screen_active() == ui_home) {
        lv_timer_delete(t);
        return;
    }

    const bool w  = boot_gate_wifi_ok();
    const bool s  = boot_gate_ws_ok();
    const bool n  = boot_gate_ntp_ok();
    const bool wd = boot_gate_weather_ok();
    const uint32_t waited = boot_gate_now_ms() - t0;

    /* ★ 天气是"软条件": 最多多等 BOOT_GATE_WEATHER_WAIT_MS, 超时就不等了。
     *   否则网关不推天气时, 开屏会一直卡到 BOOT_GATE_MAX_WAIT_MS (20s) ✗ */
    const bool weather_given_up = (waited >= BOOT_GATE_WEATHER_WAIT_MS);
    const bool ready = w && s && n && (wd || weather_given_up);

    /* ★ 两个"还不能切"的条件:
     *   ① 开屏动画还没播完 (waited < MIN) —— 掐断动画很难看
     *   ② 四项配置还没齐
     * 只要占一个就继续等。 */
    const bool anim_done = (waited >= BOOT_GATE_MIN_WAIT_MS);

    if (!anim_done || !ready) {
        /* 动画播完之后才开始提示"卡在哪一步" ——
         * 播完前改文字的话, loadingtext 的淡入动画还没走完就被我们覆盖了。
         * ⚠️ ui_loadingtext 用的是 lv_font_montserrat_16 (只有 ASCII), 所以只能用英文。
         * ★ 顺序 = 依赖顺序: 先连上 WiFi 才谈得上网关, 网关通了才有天气, 对时是独立的。 */
        if (anim_done && ui_loadingtext != NULL && lv_obj_is_valid(ui_loadingtext)) {
            const char *hint = !w  ? "wifi..."
                             : !s  ? "gateway..."
                             : !n  ? "time..."
                             : "weather...";     /* 只剩天气没到 (超时后就不会走到这了) */
            lv_label_set_text(ui_loadingtext, hint);
        }

        if (waited < BOOT_GATE_MAX_WAIT_MS) {
            return;                      /* 继续等 */
        }
        ESP_LOGW(TAG, "启动门: 等满 %ums 仍未全就绪 (wifi=%d ws=%d ntp=%d weather=%d), 强制切换",
                 BOOT_GATE_MAX_WAIT_MS, (int)w, (int)s, (int)n, (int)wd);
    } else {
        ESP_LOGI(TAG, "启动门: 动画已播完 + 四项就绪 (wifi=%d ws=%d ntp=%d weather=%d%s), 等了 %ums 就切主界面",
                 (int)w, (int)s, (int)n, (int)wd,
                 wd ? "" : " [天气超时, 不等了]", waited);
    }

    /* 切到主界面: 动画 500ms, delay 0 (立刻开始)。
     * ★ 不能传 NULL 作为 target —— ui_home 此刻已经建好了 (ui_init 里就建了)。 */
    _ui_screen_change(&ui_home, LV_SCREEN_LOAD_ANIM_FADE_ON, 500, 0, &ui_home_screen_init);
    lv_timer_delete(t);
}

esp_err_t lvgl_port_boot_gate_start(void)
{
    if (lv_timer_create(boot_gate_cb, BOOT_GATE_PERIOD_MS, NULL) == NULL) {
        ESP_LOGE(TAG, "启动门 timer 创建失败 —— 只能靠 ui_main.c 的 7 秒兜底了");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "启动门已启动 (每 %dms 查一次, 至少等 %dms 让动画播完, 最多 %dms)",
             BOOT_GATE_PERIOD_MS, BOOT_GATE_MIN_WAIT_MS, BOOT_GATE_MAX_WAIT_MS);
    return ESP_OK;
}
