// main/kugou_model.h —— 酷狗音乐播放器的纯逻辑层。
//
// 这一层不依赖 ESP-IDF / LVGL / FreeRTOS：页面状态机、三键映射、播放顺序
// （顺序循环 / 随机不重复）、进度推进与文案都在这里，便于在 PC 上做主机测试。
// 音频、网络、屏幕等副作用由调用方根据返回的 kg_action_t 执行。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define KG_NAME_CAP        40
#define KG_MSG_CAP         48
#define KG_PLAYLIST_MAX     8
#define KG_TRACK_MAX       96
#define KG_TRACKS_PER_LIST 12
#define KG_PAGE_SIZE        4   // 歌单/歌曲每批加载 4 个
#define KG_AP_MAX          12
#define KG_SSID_CAP        33
#define KG_PASS_CAP        64

typedef enum {
    KG_PAGE_LOGIN = 0,
    KG_PAGE_WIFI,           // 配网：扫描到的 AP 列表
    KG_PAGE_KEYBOARD,       // 配网：三键屏幕键盘输密码
    KG_PAGE_PLAYLISTS,
    KG_PAGE_TRACKLIST,
    KG_PAGE_PLAYER,
} kg_page_t;

// 与平台无关的 AP 视图（网络层填充，UI 只读）
typedef struct {
    char ssid[KG_SSID_CAP];
    int8_t rssi;
    bool open;
} kg_ap_view_t;

typedef enum {
    KG_LOGIN_WAIT = 0,      // 等待扫码
    KG_LOGIN_SCANNED,       // 已扫码，等待确认
    KG_LOGIN_DONE,          // 授权成功
    KG_LOGIN_EXPIRED,       // 二维码过期
    KG_LOGIN_ERROR,         // 网络/接口错误
} kg_login_t;

typedef enum {
    KG_NET_OFFLINE = 0,     // 未配置网络，使用本地演示曲库
    KG_NET_CONNECTING,
    KG_NET_ONLINE,
    KG_NET_ERROR,
} kg_net_t;

typedef enum { KG_KEY_UP = 0, KG_KEY_DOWN, KG_KEY_OK } kg_key_t;
typedef enum { KG_KEY_CLICK = 0, KG_KEY_LONG } kg_key_ev_t;

typedef enum {
    KG_ACT_NONE = 0,
    KG_ACT_REPAINT,         // 仅重绘
    KG_ACT_TRACK_CHANGED,   // 曲目变化：重启合成旋律
    KG_ACT_PLAY_TOGGLE,     // 播放/暂停：暂停或恢复合成旋律
    KG_ACT_LOGIN_START,     // 请求网络层开始扫码状态轮询
    KG_ACT_LOGIN_REFRESH,   // 请求网络层重新取二维码
    KG_ACT_LOGOUT,          // 退出登录：清理会话
    KG_ACT_WIFI_SCAN,       // 重新扫描 Wi-Fi
    KG_ACT_WIFI_CONNECT,    // 用当前 SSID/密码连接
    KG_ACT_URL_REQUEST,     // 请求当前曲目的在线播放地址
    KG_ACT_LOAD_MORE_PLAYLISTS, // 歌单翻页：加载下一批歌单
    KG_ACT_LOAD_MORE_TRACKS,    // 曲目翻页：加载下一批歌曲
} kg_action_t;

typedef struct {
    char title[KG_NAME_CAP];
    char artist[KG_NAME_CAP];
    char hash[40];          // 在线曲目的酷狗 hash，离线演示为空
    char album_audio_id[24];
    uint16_t duration_s;
    uint8_t seed;           // 离线兜底旋律的种子
} kg_track_t;

typedef struct {
    char name[KG_NAME_CAP];
    char subtitle[KG_NAME_CAP];
    char collection_id[40]; // 在线歌单 id，离线演示为空
    uint32_t accent;        // 0xRRGGBB，歌单卡片主色
    uint8_t first;          // 曲目池起始下标
    uint8_t count;          // 已加载的曲目数
    uint16_t total;         // 歌单总曲目数（用于判断是否还有下一批）
} kg_playlist_t;

typedef struct {
    uint8_t playlist_count;
    uint8_t track_count;
    kg_playlist_t playlists[KG_PLAYLIST_MAX];
    kg_track_t tracks[KG_TRACK_MAX];
} kg_catalog_t;

typedef struct {
    kg_page_t page;
    kg_login_t login;
    kg_net_t net;
    bool playing;
    bool demo_login;        // 离线演示：跳过真实扫码
    uint8_t playlist_cursor;
    uint8_t track_cursor;   // 当前歌单内的曲目序号
    int current_track;      // 曲目池全局下标，-1 表示未加载
    uint32_t elapsed_s;
    uint8_t batch_played;   // 当前批次已顺序播放的曲目数，满 4 触发加载下一批
    int battery;
    char message[KG_MSG_CAP];
    // 配网与在线播放
    kg_ap_view_t aps[KG_AP_MAX];
    uint8_t ap_count;
    uint8_t ap_cursor;
    char ssid[KG_SSID_CAP];
    char password[KG_PASS_CAP];
    uint8_t kb_group;
    uint8_t kb_index;
    bool online_catalog;    // 曲库来自酷狗账号而非内置演示数据
    bool waiting_url;       // 正在等待播放地址
    bool playlists_done;    // 歌单已全部加载完，翻页不再请求
    bool pending_play;      // 已切到未加载曲目，等下一批到达后自动续播
    char nickname[KG_NAME_CAP];
} kg_state_t;

// ---- 曲库 -----------------------------------------------------------------
void kg_catalog_default(kg_catalog_t *c);
uint8_t kg_list_size(const kg_catalog_t *c, uint8_t pl);
int kg_list_track(const kg_catalog_t *c, uint8_t pl, uint8_t idx);
const kg_track_t *kg_loaded_track(const kg_catalog_t *c, const kg_state_t *s);

// ---- 状态机 ---------------------------------------------------------------
void kg_model_init(kg_state_t *s, const kg_catalog_t *c);
kg_action_t kg_model_key(kg_state_t *s, const kg_catalog_t *c,
                         kg_key_t key, kg_key_ev_t ev);
kg_action_t kg_model_tick(kg_state_t *s, const kg_catalog_t *c, uint32_t seconds);

void kg_model_set_login(kg_state_t *s, kg_login_t login);
void kg_model_set_net(kg_state_t *s, kg_net_t net);
void kg_model_set_message(kg_state_t *s, const char *message);
void kg_model_set_battery(kg_state_t *s, int soc);

// 文案统一在这里，UI 只负责画；返回值是静态字符串，无需释放。
const char *kg_model_mode_label(const kg_state_t *s);
const char *kg_model_login_label(const kg_state_t *s);
const char *kg_model_net_label(const kg_state_t *s);
