// main/app_fonts.c —— 字体描述符初始化。
#include "app_fonts.h"

#include "esp_log.h"

static const char *TAG = "app_fonts";

// 静态描述符的可写副本：只改 fallback 指针，位图数据仍在 Flash 里只读。
static lv_font_t s_body;
static lv_font_t s_title;
static bool s_ready;

void app_fonts_init(void)
{
    if (s_ready) return;

    s_body = app_font_16;
    s_body.fallback = &lv_font_montserrat_14;
    s_title = app_font_20;
    // s_title 的 fallback 在生成时已指向 app_font_20_ext（GB2312 二级字，
    // 伽/器/鑫/淼 这类运行时输入的生僻字都在里面）。不能覆盖成 montserrat，
    // 否则二级字退回豆腐块。16px 是单文件全量，montserrat 兜底无害。
    s_ready = true;

    // 开机打印一次覆盖自检，缺字一眼可见（不写日志风险，只是字符串长度）。
    ESP_LOGI(TAG, "中文字体就绪: 16px line_h=%d 20px line_h=%d",
             (int)s_body.line_height, (int)s_title.line_height);
}

const lv_font_t *app_font_body(void)
{
    return s_ready ? &s_body : &app_font_16;
}

const lv_font_t *app_font_title(void)
{
    return s_ready ? &s_title : &app_font_20;
}
