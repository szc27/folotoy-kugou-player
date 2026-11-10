// components/bsp/src/bsp_display_lvgl.c
// LVGL 接入单独成文件:不用 LVGL 的开发者删掉本文件 + idf_component.yml 里的两条依赖即可。
#include "bsp_display.h"
#include "bsp_display_rounding.h"
#include "bsp_pins.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "bsp_lvgl";

#define BSP_LVGL_DRAW_BUFFER_LINES 20

static lv_display_t *s_disp;
static bool s_port_initialized;
static bool s_port_init_failed;

static void rounded_flush_event(lv_event_t *event)
{
    lv_display_t *disp = lv_event_get_target(event);
    const lv_area_t *area = lv_event_get_param(event);
    lv_draw_buf_t *draw_buf = lv_display_get_buf_active(disp);
    if (!area || !draw_buf || !draw_buf->data ||
        lv_display_get_color_format(disp) != LV_COLOR_FORMAT_RGB565) {
        return;
    }

    const int32_t width = lv_area_get_width(area);
    if (draw_buf->header.stride < (uint32_t)width * sizeof(uint16_t)) return;

    for (int32_t y = area->y1; y <= area->y2; ++y) {
        uint16_t *row = (uint16_t *)(draw_buf->data +
                                     (y - area->y1) * draw_buf->header.stride);
        int32_t visible_x1;
        int32_t visible_x2;
        if (!bsp_display_rounded_row_span(y, BSP_LCD_W, BSP_LCD_H,
                                          BSP_LVGL_SCREEN_RADIUS, &visible_x1,
                                          &visible_x2)) {
            memset(row, 0, (size_t)width * sizeof(uint16_t));
            continue;
        }
        // Only clear pixels outside the visible span. The port swaps RGB565
        // bytes after this event; black is 0 in either byte order.
        const int32_t clear_left_end = visible_x1 > area->x2 ? area->x2 : visible_x1 - 1;
        const int32_t clear_right_start = visible_x2 < area->x1 ? area->x1 : visible_x2 + 1;
        for (int32_t x = area->x1; x <= clear_left_end; ++x) {
            row[x - area->x1] = 0;
        }
        for (int32_t x = clear_right_start; x <= area->x2; ++x) {
            row[x - area->x1] = 0;
        }
    }

}

lv_display_t *bsp_lvgl_init(void) {
    if (s_disp) return s_disp;
    if (!bsp_display_panel()) {
        ESP_LOGE(TAG, "请先成功调用 bsp_display_init()");
        return NULL;
    }

    if (!s_port_initialized) {
        // Port 2.9.0 has no public completion handshake for asynchronous deinit.
        // Never overwrite a possibly live context after a partial port failure.
        if (s_port_init_failed) {
            ESP_LOGE(TAG, "LVGL port 初始化未完成，需重启后重试");
            return NULL;
        }
        const lvgl_port_cfg_t pc = ESP_LVGL_PORT_INIT_CONFIG();
        if (lvgl_port_init(&pc) != ESP_OK) {
            s_port_init_failed = true;
            ESP_LOGE(TAG, "lvgl_port_init 失败，需重启后重试");
            return NULL;
        }
        s_port_initialized = true;
    }

    const lvgl_port_display_cfg_t dc = {
        .panel_handle = bsp_display_panel(),
        .io_handle    = bsp_display_io(),
        // ⚠ C3 无 PSRAM,DMA 只能用内部 RAM。20 行单缓冲约 9.6KB，
        // 给 Wi-Fi 的 static RX buffer 留出 DMA 连续内存(其硬性需 6×1600)。
        .buffer_size   = (uint32_t)BSP_LCD_W * BSP_LVGL_DRAW_BUFFER_LINES,
        .double_buffer = false,
        .hres = BSP_LCD_W, .vres = BSP_LCD_H,
        // 旋转/镜像必须在这里配:esp_lvgl_port 注册显示时会重新下发 MADCTL,
        // 覆盖 bsp_display.c 里 esp_lcd_panel_mirror() 的设置。
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        // swap_bytes:LVGL 输出小端 RGB565,ST7789 走 SPI 要大端 → 需交换高低字节。
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    // The port mutex is recursive. Keep registration and the mask callback in
    // one critical section, before the new display can produce its first flush.
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "LVGL 初始化加锁失败");
        return NULL;
    }
    lv_display_t *disp = lvgl_port_add_disp(&dc);
    bool mask_registered = false;
    if (disp) {
        // LVGL 9.5 returns void here; check the list while still holding the lock.
        const uint32_t count = lv_display_get_event_count(disp);
        lv_display_add_event_cb(disp, rounded_flush_event, LV_EVENT_FLUSH_START, NULL);
        mask_registered = lv_display_get_event_count(disp) == count + 1;
    }
    if (!mask_registered) {
        ESP_LOGE(TAG, "LVGL display 或圆角回调注册失败");
        if (disp) lvgl_port_remove_disp(disp);
        lvgl_port_unlock();
        // Retain the initialized port for retry. Deinit is asynchronous and can
        // race the next init (or even run before the task sets running=true).
        return NULL;
    }

    // Mask the final RGB565 flush instead of using root-screen clip_corner.
    // Full-screen rounded clipping creates an ARGB layer that does not fit the
    // 24 KB LVGL pool reliably on this no-PSRAM target.
    s_disp = disp;
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LVGL 就绪，全局圆角=%d，外部填充=黑色", BSP_LVGL_SCREEN_RADIUS);
    return s_disp;
}

bool bsp_lvgl_lock(int timeout_ms) {
    if (!s_disp) return false;
    return lvgl_port_lock(timeout_ms);
}
void bsp_lvgl_unlock(void) {
    if (s_disp) lvgl_port_unlock();
}

// ---------------------------------------------------------------------------
// 浅睡眠：只关屏，不释放 SPI/背光资源，CPU 与外设照常运行（BLE 连接不断）。
// 用于「无操作 N 秒熄屏」，区别于 deep sleep 那套「关完必须立刻睡」的流程。
// ---------------------------------------------------------------------------
static bool s_screen_awake = true;

bool bsp_display_is_awake(void) {
    return s_screen_awake;
}

esp_err_t bsp_display_sleep(void) {
    esp_lcd_panel_handle_t panel = bsp_display_panel();
    if (!s_disp || !panel) {
        ESP_LOGE(TAG, "浅睡眠失败：显示未就绪");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_screen_awake) return ESP_OK;   // 已在息屏态

    if (!bsp_lvgl_lock(500)) {
        ESP_LOGE(TAG, "浅睡眠加锁超时");
        return ESP_ERR_TIMEOUT;
    }
    // 先停 LVGL 输出，再让面板进睡眠：否则面板睡着后 LVGL 还在推 SPI，
    // 白白烧总线功耗，而且面板睡眠期间写入会被忽略。
    lv_display_enable_invalidation(s_disp, false);
    esp_lcd_panel_disp_on_off(panel, false);
    esp_lcd_panel_disp_sleep(panel, true);   // SLPIN，面板功耗降到微安级
    bsp_display_backlight(0);
    s_screen_awake = false;
    bsp_lvgl_unlock();
    ESP_LOGI(TAG, "已熄屏（背光灭 + ST7789 Sleep In，CPU 与 BLE 保持运行）");
    return ESP_OK;
}

esp_err_t bsp_display_wake(void) {
    esp_lcd_panel_handle_t panel = bsp_display_panel();
    if (!s_disp || !panel) {
        ESP_LOGE(TAG, "唤醒失败：显示未就绪");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_screen_awake) return ESP_OK;   // 已在亮屏态

    if (!bsp_lvgl_lock(500)) {
        ESP_LOGE(TAG, "唤醒加锁超时");
        return ESP_ERR_TIMEOUT;
    }
    bsp_display_backlight(100);                 // 先亮背光，唤醒过程肉眼可见
    esp_lcd_panel_disp_sleep(panel, false);     // SLPOUT（驱动内部自带唤醒延时）
    esp_lcd_panel_disp_on_off(panel, true);     // DISPON
    lv_display_enable_invalidation(s_disp, true);   // 恢复 LVGL 输出
    lv_obj_invalidate(lv_screen_active());      // 全屏重绘，避免残留旧帧
    s_screen_awake = true;
    bsp_lvgl_unlock();
    ESP_LOGI(TAG, "已亮屏（SLPOUT + DISPON，全屏重绘）");
    return ESP_OK;
}
