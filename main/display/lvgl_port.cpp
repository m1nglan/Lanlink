#include "display/lvgl_port.hpp"
#include "display/lcd_display.hpp"
#include "drivers/encoder.hpp"

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/spi_master.h"

#include "lvgl.h"

static const char *TAG = "lvgl";

/* ------------------ 参数 ------------------ */
#define LVGL_SPI_HOST       (SPI2_HOST)
#define LVGL_TICK_PERIOD_MS (2)
#define LVGL_RENDER_FULL    (1)         /*!< 1=双整屏缓冲(FULL), 0=partial 行缓冲 */
#define LVGL_BUF_IN_PSRAM   (1)         /*!< 1=绘图缓冲放 PSRAM (需 SPI2+GDMA 直读, 自动 cache sync), 0=内部 DMA SRAM */
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
    uint32_t wait_ms = 0;
    while (1) {
        lvgl_port_lock();
        wait_ms = lv_timer_handler();
        lvgl_port_unlock();
        if (wait_ms < 5) {
            wait_ms = 5;      /* 避免忙循环空转 */
        }
        if (wait_ms > 500) {
            wait_ms = 500;    /* 避免长时间不调度 */
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
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

    /* 4. 绘图缓冲
     *    LVGL_RENDER_FULL=1: 双整屏缓冲(FULL 模式) 320*170*2 = ~106KB x2
     *    LVGL_RENDER_FULL=0: partial 行缓冲(每缓冲 LCD_H_RES*LVGL_PARTIAL_LINES)
     *    LVGL_BUF_IN_PSRAM=1: 缓冲放 PSRAM (SPI2 + GDMA 直读, 驱动自动 cache sync/对齐) */
#if LVGL_RENDER_FULL
    size_t buf_sz = LCD_H_RES * LCD_V_RES * sizeof(lv_color16_t);
    lv_display_render_mode_t render_mode = LV_DISPLAY_RENDER_MODE_FULL;
#else
    #define LVGL_PARTIAL_LINES (40)
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

/* 确保"当前激活屏幕"是默认 group 的聚焦对象 (无锁, 须在持锁上下文调用)。
 * SquareLine 把切屏事件挂在屏幕对象本身, 只有被聚焦的屏幕才收得到 LV_EVENT_KEY。 */
static void lvgl_focus_active_screen_locked(lv_group_t *g)
{
    lv_obj_t *scr = lv_screen_active();
    if (scr == NULL) {
        return;
    }
    if (lv_obj_get_group(scr) != g) {
        lv_group_add_obj(g, scr);
    }
    if (lv_group_get_focused(g) != scr) {
        lv_group_focus_obj(scr);
    }
}

void lvgl_port_send_encoder_dir(int dir)
{
    lvgl_port_lock();

    lv_group_t *g = lv_group_get_default();
    if (g == NULL) {
        g = lv_group_create();
        lv_group_set_default(g);
    }
    lvgl_focus_active_screen_locked(g);
    lv_group_send_data(g, (dir < 0) ? LV_KEY_LEFT : LV_KEY_RIGHT);

    lvgl_port_unlock();
}

/* ============ 编码器 indev (无任务, 由 lv_timer_handler 周期驱动) ============
 * LVGL 内部每轮 lv_timer_handler 会调用本 read_cb (已在持锁上下文, 禁止再加锁)。
 * 我们从 encoder 驱动取原始跳变, 按 ENC_KEY_STEP 换算成"格"填入 data->enc_diff。 */

static int      s_enc_carry = 0;    /*!< 不足一格的跳变余数 (跨帧累积) */

static void lvgl_encoder_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    int32_t raw = encoder_consume_raw();      /* 取走全部净跳变(含正负) */
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

    /* 2. ★ 编辑模式: 让旋转发 LV_KEY_LEFT/RIGHT 给聚焦对象,
     *    而不是移动焦点 (导航模式)。SquareLine 的切屏事件监听的是前者。 */
    lv_group_set_editing(g, true);

    /* 3. 建 indev 并绑定 */
    lv_indev_t *indev = lv_indev_create();
    if (indev == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_indev_set_type(indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(indev, lvgl_encoder_read_cb);
    lv_indev_set_group(indev, g);

    lvgl_focus_active_screen_locked(g);

    ESP_LOGI(TAG, "encoder indev 注册完成 (编辑模式, 发 LV_KEY_LEFT/RIGHT)");
    return ESP_OK;
}
