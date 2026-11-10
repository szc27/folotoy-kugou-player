// main/kugou_main.c —— 酷狗直连播放器入口与事件循环。
//
// 单线程模型：按键、网络结果、周期 tick 全部进同一条队列，由 app_task 处理，
// 因此状态与曲库不需要锁，LVGL 也只在这条任务里访问（内部仍持 bsp_lvgl_lock）。
#include <stdio.h>
#include <string.h>

#include "app_fonts.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "kugou_api.h"
#include "kugou_keyboard.h"
#include "kugou_model.h"
#include "kugou_stream.h"
#include "kugou_ui.h"

static const char *TAG = "kg_app";

// 酷狗音乐播放器应用版本号（发布时随 tag 同步更新）。
#define KG_APP_VERSION "1.0.0"

#define EVENT_QUEUE_DEPTH 4
#define TICK_PERIOD_US    (1000 * 1000)
#define BATTERY_TICKS     30

typedef enum { APP_EV_KEY = 0, APP_EV_NET, APP_EV_TICK } app_ev_type_t;

typedef struct {
    app_ev_type_t type;
    union {
        struct { int key; int ev; } key;
        kg_event_t net;
    } u;
} app_event_t;

static kg_catalog_t s_catalog;
static kg_state_t s_state;
static kg_keyboard_t s_kb;
static QueueHandle_t s_queue;
static int s_ticks;
static bool s_wifi_prompted;

// ---------------------------------------------------------------------------
// 回调：只入队
// ---------------------------------------------------------------------------

static void on_button(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_LONG) return;
    app_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = APP_EV_KEY;
    event.u.key.key = (int)btn;
    event.u.key.ev = (ev == BSP_BTN_LONG) ? (int)KG_KEY_LONG : (int)KG_KEY_CLICK;
    if (s_queue) (void)xQueueSend(s_queue, &event, 0);
}

static void on_net_event(const kg_event_t *ev, void *user)
{
    (void)user;
    if (!ev || !s_queue) return;
    app_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = APP_EV_NET;
    event.u.net = *ev;
    (void)xQueueSend(s_queue, &event, pdMS_TO_TICKS(50));
}

static void on_tick(void *arg)
{
    (void)arg;
    app_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = APP_EV_TICK;
    if (s_queue) (void)xQueueSend(s_queue, &event, 0);
}

// ---------------------------------------------------------------------------
// 命令辅助
// ---------------------------------------------------------------------------

static void send_simple(kg_cmd_type_t type)
{
    kg_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = type;
    kg_api_send(&cmd);
}

static void request_track_url(void)
{
    const kg_track_t *t = kg_loaded_track(&s_catalog, &s_state);
    if (!t) return;
    if (!t->hash[0]) {
        kg_model_set_message(&s_state, "演示曲目 无在线音频");
        return;  // 离线演示：不请求，只走 UI 进度
    }
    kg_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = KG_CMD_TRACK_URL;
    snprintf(cmd.key, sizeof(cmd.key), "%s", t->hash);
    snprintf(cmd.extra, sizeof(cmd.extra), "%s", t->album_audio_id);
    cmd.track_index = s_state.track_cursor;
    s_state.waiting_url = true;
    kg_model_set_message(&s_state, "正在获取播放地址");
    kg_api_send(&cmd);
}

static void request_tracks(uint8_t playlist)
{
    if (!s_state.online_catalog || playlist >= s_catalog.playlist_count) return;
    const char *id = s_catalog.playlists[playlist].collection_id;
    if (!id || !id[0]) return;
    kg_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = KG_CMD_TRACKS;
    snprintf(cmd.extra, sizeof(cmd.extra), "%s", id);
    cmd.playlist_index = playlist;
    kg_model_set_message(&s_state, "正在载入歌单曲目");
    kg_api_send(&cmd);
}

// 选中并请求播放某首已加载曲目（在线播放路径）。
static void play_track_at(uint8_t idx)
{
    s_state.track_cursor = idx;
    s_state.current_track = kg_list_track(&s_catalog, s_state.playlist_cursor, idx);
    s_state.elapsed_s = 0;
    request_track_url();
}

// 在线自然播完一首后的顺序推进：优先切已加载的下一首；已到本批末尾且还有
// 更多时加载下一批并标记续播；否则回到第一首循环。
static void advance_online(void)
{
    uint8_t pl = s_state.playlist_cursor;
    if (pl >= s_catalog.playlist_count) return;
    const kg_playlist_t *list = &s_catalog.playlists[pl];
    uint8_t next = (uint8_t)(s_state.track_cursor + 1);
    if (next < list->count) {
        play_track_at(next);
    } else if (list->count < list->total && list->count < KG_TRACKS_PER_LIST) {
        s_state.track_cursor = next;
        s_state.current_track = -1;
        s_state.pending_play = true;
        request_tracks(pl);
    } else {
        play_track_at(0);
    }
}

// ---------------------------------------------------------------------------
// 事件处理
// ---------------------------------------------------------------------------

static void handle_key(const app_event_t *event)
{
    kg_key_t key = (kg_key_t)event->u.key.key;
    kg_key_ev_t ev = (kg_key_ev_t)event->u.key.ev;
    kg_page_t prev = s_state.page;

    // 配网键盘页：单击/长按统一交给纯逻辑键盘模块，
    // 由其返回 SUBMIT(提交)/CANCEL(空密码取消)/CHANGED(重绘)。
    if (s_state.page == KG_PAGE_KEYBOARD) {
        kg_kb_action_t kb_act = kg_keyboard_key(&s_kb, key, ev);
        s_state.kb_group = s_kb.group;
        s_state.kb_index = s_kb.index;
        snprintf(s_state.password, sizeof(s_state.password), "%s", s_kb.text);

        if (kb_act == KG_KB_SUBMIT) {
            kg_cmd_t cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.type = KG_CMD_CONNECT;
            snprintf(cmd.ssid, sizeof(cmd.ssid), "%s", s_state.ssid);
            snprintf(cmd.pass, sizeof(cmd.pass), "%s", s_state.password);
            kg_api_send(&cmd);
        } else if (kb_act == KG_KB_CANCEL) {
            s_state.page = KG_PAGE_WIFI;  // 空密码长按确认：返回配网页
        }
        kg_ui_render(&s_state, &s_catalog);
        return;
    }

    kg_action_t action = kg_model_key(&s_state, &s_catalog, key, ev);

    if (s_state.page == KG_PAGE_KEYBOARD && prev != KG_PAGE_KEYBOARD) {
        kg_keyboard_init(&s_kb);
        s_state.kb_group = s_kb.group;
        s_state.kb_index = s_kb.index;
        s_state.password[0] = '\0';
    }

    switch (action) {
    case KG_ACT_TRACK_CHANGED:
    case KG_ACT_URL_REQUEST:
        if (s_state.online_catalog) request_track_url();
        break;
    case KG_ACT_PLAY_TOGGLE:
        kg_stream_pause(!s_state.playing);
        break;
    case KG_ACT_LOGIN_START:
        send_simple(KG_CMD_QR_START);
        break;
    case KG_ACT_LOGIN_REFRESH:
        send_simple(KG_CMD_QR_REFRESH);
        break;
    case KG_ACT_LOGOUT:
        kg_stream_stop();
        // 退出登录：回到离线演示态，避免在线推进逻辑在无会话时继续触发
        s_state.online_catalog = false;
        s_state.waiting_url = false;
        s_state.pending_play = false;
        s_state.playlists_done = false;
        kg_catalog_default(&s_catalog);
        s_state.playlist_cursor = 0;
        s_state.track_cursor = 0;
        s_state.current_track = kg_list_track(&s_catalog, 0, 0);
        send_simple(KG_CMD_LOGOUT);
        break;
    case KG_ACT_WIFI_SCAN:
        send_simple(KG_CMD_SCAN);
        break;
    case KG_ACT_WIFI_CONNECT: {
        kg_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = KG_CMD_CONNECT;
        snprintf(cmd.ssid, sizeof(cmd.ssid), "%s", s_state.ssid);
        snprintf(cmd.pass, sizeof(cmd.pass), "%s", s_state.password);
        kg_api_send(&cmd);
        break;
    }
    case KG_ACT_LOAD_MORE_PLAYLISTS:
        send_simple(KG_CMD_PLAYLISTS);
        break;
    case KG_ACT_LOAD_MORE_TRACKS:
        request_tracks(s_state.playlist_cursor);
        break;
    default:
        break;
    }

    // 从歌单页首次进入曲目页时拉取第一批在线曲目（后续批次靠翻页/续播触发）
    if (s_state.page == KG_PAGE_TRACKLIST && prev == KG_PAGE_PLAYLISTS &&
        s_state.online_catalog &&
        s_state.playlist_cursor < s_catalog.playlist_count &&
        s_catalog.playlists[s_state.playlist_cursor].count == 0) {
        request_tracks(s_state.playlist_cursor);
    }
    if (s_state.page == KG_PAGE_PLAYER && !s_state.playing) {
        kg_stream_pause(true);
    }
    kg_ui_render(&s_state, &s_catalog);
}

static void handle_net(const kg_event_t *ev)
{
    switch (ev->type) {
    case KG_EV_WIFI:
        switch (ev->wifi) {
        case KG_WIFI_READY:
            kg_model_set_net(&s_state, KG_NET_ONLINE);
            // 连上网络后，若还在配网/键盘页，自动切到扫码登录页
            if (s_state.page == KG_PAGE_WIFI ||
                s_state.page == KG_PAGE_KEYBOARD) {
                s_state.page = KG_PAGE_LOGIN;
            }
            break;
        case KG_WIFI_CONNECTING:kg_model_set_net(&s_state, KG_NET_CONNECTING); break;
        case KG_WIFI_FAILED:    kg_model_set_net(&s_state, KG_NET_ERROR); break;
        default:                kg_model_set_net(&s_state, KG_NET_OFFLINE); break;
        }
        if (ev->ap_count > 0) {
            memcpy(s_state.aps, ev->aps,
                   ev->ap_count * sizeof(kg_ap_view_t));
            s_state.ap_count = ev->ap_count;
            if (s_state.ap_cursor >= s_state.ap_count) s_state.ap_cursor = 0;
        }
        if (ev->message[0]) kg_model_set_message(&s_state, ev->message);
        // 首次未配置网络时自动进入配网页；已登录期间不打断播放
        if (!s_wifi_prompted && ev->wifi == KG_WIFI_IDLE &&
            s_state.page == KG_PAGE_LOGIN) {
            s_wifi_prompted = true;
            s_state.page = KG_PAGE_WIFI;
            send_simple(KG_CMD_SCAN);
        }
        break;
    case KG_EV_QR:
        kg_ui_set_qr(ev->qr_url);
        break;
    case KG_EV_LOGIN:
        kg_model_set_login(&s_state, ev->login);
        if (ev->nickname[0]) {
            snprintf(s_state.nickname, sizeof(s_state.nickname), "%s", ev->nickname);
        }
        if (ev->login == KG_LOGIN_DONE) {
            s_wifi_prompted = true;
            if (s_state.page == KG_PAGE_LOGIN ||
                s_state.page == KG_PAGE_WIFI ||
                s_state.page == KG_PAGE_KEYBOARD) {
                s_state.page = KG_PAGE_PLAYLISTS;
            }
        }
        break;
    case KG_EV_CATALOG:
        s_catalog = *kg_api_catalog();
        s_state.online_catalog = true;
        if (ev->playlist_index >= 0 &&
            ev->playlist_index < s_catalog.playlist_count) {
            s_state.playlist_cursor = (uint8_t)ev->playlist_index;
        }
        if (ev->exhausted && ev->playlist_index < 0) {
            // 歌单拉取到底：翻页终止
            s_state.playlists_done = true;
            kg_model_set_message(&s_state, "没有更多歌单");
        } else if (ev->playlist_index >= 0) {
            // 曲目批次到达：把当前曲目指向光标位置；若在等续播则立即请求播放
            uint8_t pl = (uint8_t)ev->playlist_index;
            if (pl < s_catalog.playlist_count &&
                s_state.track_cursor < s_catalog.playlists[pl].count) {
                s_state.current_track =
                    kg_list_track(&s_catalog, pl, s_state.track_cursor);
            } else {
                s_state.track_cursor = 0;
                s_state.current_track = kg_list_track(&s_catalog, pl, 0);
            }
            if (s_state.pending_play) {
                s_state.pending_play = false;
                if (s_state.current_track >= 0) request_track_url();
            }
            kg_model_set_message(&s_state, "已载入曲目");
        } else {
            // 歌单批次到达：光标与曲目不动
            kg_model_set_message(&s_state, "已载入在线歌单");
        }
        ESP_LOGI(TAG, "online catalog: %d lists / %d tracks",
                 (int)s_catalog.playlist_count, (int)s_catalog.track_count);
        break;
    case KG_EV_URL:
        s_state.waiting_url = false;
        if (ev->stream_url[0]) {
            kg_stream_play(ev->stream_url);
            kg_stream_pause(!s_state.playing);
            kg_model_set_message(&s_state, "正在缓冲");
        } else {
            kg_model_set_message(&s_state,
                                 ev->message[0] ? ev->message : "无播放地址 可能是版权限制");
        }
        break;
    case KG_EV_MESSAGE:
        kg_model_set_message(&s_state, ev->message);
        break;
    default:
        break;
    }
    kg_ui_render(&s_state, &s_catalog);
}

static void handle_tick(void)
{
    s_ticks++;
    bool changed = false;

    if (s_state.online_catalog) {
        // 在线播放：进度来自流，播完由 EOF 驱动顺序推进（独立于 active 状态）
        if (s_state.playing && kg_stream_active()) {
            s_state.elapsed_s = kg_stream_elapsed_ms() / 1000u;
            changed = true;
        }
        // 用 waiting_url / pending_play 防重入：从 EOF 到拿到新地址或新批次
        // 之间，kg_stream_eof() 会持续为真，避免每个 tick 都触发一次推进。
        if (kg_stream_eof() && !s_state.waiting_url && !s_state.pending_play) {
            s_state.batch_played++;
            advance_online();
            changed = true;
        }
    } else if (kg_model_tick(&s_state, &s_catalog, 1) != KG_ACT_NONE) {
        changed = true;
    }

    if (s_ticks % BATTERY_TICKS == 0) {
        int soc = bsp_battery_soc();
        if (soc != s_state.battery) {
            kg_model_set_battery(&s_state, soc);
            changed = true;
        }
    }
    if (changed) kg_ui_render(&s_state, &s_catalog);
}

static void app_task(void *arg)
{
    (void)arg;
    app_event_t event;
    while (1) {
        if (xQueueReceive(s_queue, &event, portMAX_DELAY) != pdTRUE) continue;
        switch (event.type) {
        case APP_EV_KEY:  handle_key(&event);       break;
        case APP_EV_NET:  handle_net(&event.u.net); break;
        case APP_EV_TICK: handle_tick();            break;
        default: break;
        }
    }
}

// ---------------------------------------------------------------------------
// 启动
// ---------------------------------------------------------------------------

void app_main(void)
{
    ESP_LOGI(TAG, "Kugou direct player v%s starting (ESP-IDF %s)",
             KG_APP_VERSION, esp_get_idf_version());

    (void)bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display or LVGL init failed");
        return;
    }
    bsp_display_backlight(100);
    app_fonts_init();

    kg_catalog_default(&s_catalog);
    kg_model_init(&s_state, &s_catalog);
    (void)bsp_battery_init();
    s_state.battery = bsp_battery_soc();

    s_queue = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(app_event_t));
    if (!s_queue) { ESP_LOGE(TAG, "queue create failed"); return; }

    kg_ui_init();
    kg_ui_render(&s_state, &s_catalog);

    // 先创建 UI/事件任务：此刻内存最充足。Wi-Fi 初始化会吃掉 ~35KB
    // （static RX 6×1600 + 各队列/driver 栈），若等 Wi-Fi init 之后再建
    // 12KB 栈的任务会因 largest block 不足而失败。任务创建后立即阻塞在空队列上。
    if (xTaskCreate(app_task, "kg_app", 12288, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "app task create failed");
        return;
    }

    if (kg_stream_init() != ESP_OK) {
        ESP_LOGW(TAG, "audio init failed, UI playback state still works");
    }
    ESP_LOGI(TAG, "after stream_init: free heap=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (kg_api_start(on_net_event, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "network worker failed to start");
    }
    ESP_LOGI(TAG, "after api_start: free heap=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (bsp_button_init(on_button, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "button init failed");
    }

    const esp_timer_create_args_t timer_args = { .callback = on_tick, .name = "kg_tick" };
    esp_timer_handle_t timer = NULL;
    if (esp_timer_create(&timer_args, &timer) == ESP_OK) {
        (void)esp_timer_start_periodic(timer, TICK_PERIOD_US);
    }

    ESP_LOGI(TAG, "ready: free heap=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}
