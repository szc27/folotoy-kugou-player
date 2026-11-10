// main/kugou_api.h —— 设备端酷狗直连客户端（无中转服务）。
//
// 一个 worker 任务独占 Wi-Fi / TLS / HTTP，结果通过回调入队给应用任务；
// 回调运行在网络上下文，只允许入队，不得访问 LVGL。
#pragma once

#include "esp_err.h"
#include "kugou_model.h"
#include "kugou_proto.h"
#include "kugou_wifi.h"

typedef enum {
    KG_EV_WIFI = 0,     // 网络状态与扫描到的 AP 列表
    KG_EV_QR,           // 二维码内容（外部 H5 登录 URL）
    KG_EV_LOGIN,        // 扫码状态变化
    KG_EV_CATALOG,      // 在线歌单 / 曲目就绪
    KG_EV_URL,          // 某首曲目的可播放地址就绪
    KG_EV_MESSAGE,      // 状态提示
} kg_ev_type_t;

typedef struct {
    kg_ev_type_t type;
    kg_wifi_state_t wifi;
    kg_ap_t aps[KG_WIFI_AP_MAX];
    uint8_t ap_count;
    kg_login_t login;
    char qr_url[KG_QR_URL_CAP];
    char message[KG_MSG_CAP];
    char nickname[KG_NAME_CAP];
    char stream_url[KG_QR_URL_CAP];
    int track_index;
    int playlist_index;
    bool exhausted;     // 本批拉取已到末尾（没有更多），供上层标记翻页终止
} kg_event_t;

typedef void (*kg_event_cb_t)(const kg_event_t *ev, void *user);

// 在线歌单/曲目不通过事件值传递（kg_catalog_t 约 8KB，值拷贝会把 worker 栈
// 压爆）。worker 维护一份 s_catalog，应用任务在收到 KG_EV_CATALOG 后通过
// 本函数读取。读写时序天然串行：worker 写完后才发事件，应用收到事件后才会
// 发出下一条拉取命令，因此无并发竞态。
const kg_catalog_t *kg_api_catalog(void);

typedef enum {
    KG_CMD_SCAN = 0,
    KG_CMD_CONNECT,
    KG_CMD_QR_START,
    KG_CMD_QR_REFRESH,
    KG_CMD_PLAYLISTS,
    KG_CMD_TRACKS,
    KG_CMD_TRACK_URL,
    KG_CMD_LOGOUT,
} kg_cmd_type_t;

typedef struct {
    kg_cmd_type_t type;
    char ssid[KG_WIFI_SSID_CAP];
    char pass[KG_WIFI_PASS_CAP];
    char key[64];        // 二维码 key / 曲目 hash
    char extra[64];      // 歌单 id / album_audio_id
    int playlist_index;
    int track_index;
} kg_cmd_t;

esp_err_t kg_api_start(kg_event_cb_t cb, void *user);
void kg_api_send(const kg_cmd_t *cmd);

// 供应用查询当前登录态（worker 内部持有，线程安全读取只做简单拷贝）。
bool kg_api_has_session(void);
