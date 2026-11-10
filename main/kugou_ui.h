// main/kugou_ui.h —— 酷狗音乐播放器的独立界面。
//
// 深色唱片风格：顶部状态栏、居中唱片、卡片列表、进度条与三键控制条。
// 不引用 demo 菜单、demo_*.c 页面或 ui_pixel 像素画外壳；只复用 BSP 的
// LVGL 初始化/加锁接口和普通 LVGL 控件。
#pragma once

#include "kugou_model.h"
#include "kugou_proto.h"

void kg_ui_init(void);

// 二维码内容（外部 H5 登录 URL）由网络层提供；页面不存在时先缓存。
void kg_ui_set_qr(const char *url);

// 由应用任务调用；内部持有 LVGL 锁。state 是唯一数据源，页面切换时重建，
// 同一页面内只更新变化的标签。
void kg_ui_render(const kg_state_t *state, const kg_catalog_t *catalog);
