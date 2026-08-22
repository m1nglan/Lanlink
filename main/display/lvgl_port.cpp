#include "display/lvgl_port.hpp"
#include "display/lcd_display.hpp"

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/spi_master.h"

#include "lvgl.h"

static const char *TAG = "lvgl";

/* ------------------ 参数 ------------------ */
#define LVGL_SPI_HOST       (SPI2_HOST)
#define LVGL_TICK_PERIOD_MS (2)
#define LVGL_DRAW_LINES     (20)        /*!< 每个绘图缓冲的行数 */
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

    /* 4. 绘图缓冲: 内部 DMA 内存, 双缓冲 partial render */
    size_t buf_sz = LCD_H_RES * LVGL_DRAW_LINES * sizeof(lv_color16_t);
    void *buf1 = spi_bus_dma_memory_alloc(LVGL_SPI_HOST, buf_sz, 0);
    void *buf2 = spi_bus_dma_memory_alloc(LVGL_SPI_HOST, buf_sz, 0);
    if (buf1 == NULL || buf2 == NULL) {
        ESP_LOGE(TAG, "draw buffer 分配失败 (%u B x2)", (unsigned)buf_sz);
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_buffers(disp, buf1, buf2, buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
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

    ESP_LOGI(TAG, "LVGL 9.5 port ready: %dx%d, %d-line draw buf (RGB565)",
             LCD_H_RES, LCD_V_RES, LVGL_DRAW_LINES);
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

void lvgl_port_send_encoder_dir(int dir)
{
    static bool s_warned = false;
    lvgl_port_lock();
    lv_group_t *g = lv_group_get_default();
    if (g == NULL) {
        if (!s_warned) {
            s_warned = true;
            ESP_LOGW(TAG, "无默认 lv_group, 编码器键未发送 (UI 导航需先建组并聚焦对象)");
        }
        lvgl_port_unlock();
        return;
    }
    lv_group_send_data(g, (dir < 0) ? LV_KEY_LEFT : LV_KEY_RIGHT);
    lvgl_port_unlock();
}
