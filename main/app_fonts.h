// main/app_fonts.h —— 中文字体描述符。
//
// 字体由 tools/gen_cjk_font.py 用 Noto Sans SC 生成（4bpp、非压缩、FORMAT0_TINY +
// SPARSE_TINY），覆盖 ASCII、常用标点与 GB2312 一级汉字。字符清单在
// assets/fonts/app_font_inventory.txt，由 tools/check_font_coverage.py 在静态
// 检查里比对 UI 字符串，新增中文字符串漏字会直接失败。
#pragma once

#include "lvgl.h"

LV_FONT_DECLARE(app_font_16);
LV_FONT_DECLARE(app_font_20);

// 在任何界面创建之前调用一次（需已初始化 LVGL）。
void app_fonts_init(void);

// 带 Montserrat 回退的字体句柄：中文字形走生成子集，LVGL 内建符号走 Montserrat。
const lv_font_t *app_font_body(void);    // 16 px，正文/列表
const lv_font_t *app_font_title(void);   // 20 px，标题
