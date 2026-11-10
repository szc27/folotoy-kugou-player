// main/kugou_wifi.h —— 设备端 Wi-Fi 扫描、连接与凭据持久化。
// 所有阻塞操作由网络 worker 任务调用，不在 LVGL / 按键回调中执行。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "kugou_model.h"

// 与 UI 共用同一份 AP 视图定义，避免结构体布局不一致导致拷贝错位。
typedef kg_ap_view_t kg_ap_t;
#define KG_WIFI_AP_MAX    KG_AP_MAX
#define KG_WIFI_SSID_CAP  KG_SSID_CAP
#define KG_WIFI_PASS_CAP  KG_PASS_CAP

typedef enum {
    KG_WIFI_IDLE = 0,
    KG_WIFI_SCANNING,
    KG_WIFI_CONNECTING,
    KG_WIFI_READY,
    KG_WIFI_FAILED,
} kg_wifi_state_t;

esp_err_t kg_wifi_init(void);
esp_err_t kg_wifi_scan(kg_ap_t *out, uint8_t *count, uint8_t cap);
esp_err_t kg_wifi_connect(const char *ssid, const char *pass);
bool kg_wifi_ready(void);
kg_wifi_state_t kg_wifi_state(void);

// NVS 命名空间 kg_wifi，键 ssid / pass。读取失败返回空串，不视为致命错误。
esp_err_t kg_wifi_load(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap);
esp_err_t kg_wifi_save(const char *ssid, const char *pass);
void kg_wifi_clear(void);
