// main/kugou_model.c —— 见 kugou_model.h。
#include "kugou_model.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// 曲库
// ---------------------------------------------------------------------------

uint8_t kg_list_size(const kg_catalog_t *c, uint8_t pl)
{
    if (!c || pl >= c->playlist_count) return 0;
    uint8_t n = c->playlists[pl].count;
    if (n > KG_TRACKS_PER_LIST) n = KG_TRACKS_PER_LIST;
    return n;
}

int kg_list_track(const kg_catalog_t *c, uint8_t pl, uint8_t idx)
{
    if (!c || pl >= c->playlist_count) return -1;
    const kg_playlist_t *list = &c->playlists[pl];
    if (idx >= list->count) return -1;
    int global = (int)list->first + (int)idx;
    if (global < 0 || global >= (int)c->track_count) return -1;
    return global;
}

const kg_track_t *kg_loaded_track(const kg_catalog_t *c, const kg_state_t *s)
{
    if (!c || !s || s->current_track < 0 ||
        s->current_track >= (int)c->track_count) {
        return NULL;
    }
    return &c->tracks[s->current_track];
}

// ---------------------------------------------------------------------------
// 随机播放：洗牌袋，保证一轮之内不重复同一首
// ---------------------------------------------------------------------------

static void load_at(kg_state_t *s, const kg_catalog_t *c, uint8_t idx)
{
    s->track_cursor = idx;
    s->current_track = kg_list_track(c, s->playlist_cursor, idx);
    s->elapsed_s = 0;
}

static void advance(kg_state_t *s, const kg_catalog_t *c, int dir)
{
    uint8_t n = kg_list_size(c, s->playlist_cursor);
    if (n == 0) {
        s->current_track = -1;
        s->elapsed_s = 0;
        return;
    }
    // 只顺序播放：上一曲/下一曲在已加载的曲目区间内循环
    int cur = (s->track_cursor < n) ? (int)s->track_cursor : 0;
    uint8_t next = (uint8_t)((cur + dir + (int)n) % (int)n);
    load_at(s, c, next);
}

// ---------------------------------------------------------------------------
// 状态机
// ---------------------------------------------------------------------------

void kg_model_init(kg_state_t *s, const kg_catalog_t *c)
{
    memset(s, 0, sizeof(*s));
    s->page = KG_PAGE_LOGIN;
    s->login = KG_LOGIN_WAIT;
    s->net = KG_NET_OFFLINE;
    s->current_track = -1;
    s->batch_played = 0;
    s->battery = -1;
    if (kg_list_size(c, 0) > 0) {
        s->current_track = kg_list_track(c, 0, 0);
    }
    kg_model_set_message(s, "请扫描二维码登录");
}

void kg_model_set_message(kg_state_t *s, const char *message)
{
    if (!message) message = "";
    snprintf(s->message, sizeof(s->message), "%s", message);
}

void kg_model_set_login(kg_state_t *s, kg_login_t login)
{
    s->login = login;
    switch (login) {
    case KG_LOGIN_WAIT:    kg_model_set_message(s, "请扫描二维码登录"); break;
    case KG_LOGIN_SCANNED: kg_model_set_message(s, "已扫码 请在手机确认"); break;
    case KG_LOGIN_DONE:    kg_model_set_message(s, "登录成功 正在进入"); break;
    case KG_LOGIN_EXPIRED: kg_model_set_message(s, "二维码已过期 请刷新"); break;
    case KG_LOGIN_ERROR:   kg_model_set_message(s, "登录失败 请检查网络"); break;
    default: break;
    }
}

void kg_model_set_net(kg_state_t *s, kg_net_t net)
{
    s->net = net;
    if (net == KG_NET_ONLINE) kg_model_set_message(s, "网络已连接");
    else if (net == KG_NET_CONNECTING) kg_model_set_message(s, "正在连接网络");
    else if (net == KG_NET_ERROR) kg_model_set_message(s, "网络连接失败");
}

void kg_model_set_battery(kg_state_t *s, int soc)
{
    s->battery = soc;
}

const char *kg_model_mode_label(const kg_state_t *s)
{
    (void)s;
    return "顺序播放";
}

const char *kg_model_login_label(const kg_state_t *s)
{
    switch (s->login) {
    case KG_LOGIN_WAIT:    return "等待扫码";
    case KG_LOGIN_SCANNED: return "等待确认";
    case KG_LOGIN_DONE:    return "已登录";
    case KG_LOGIN_EXPIRED: return "二维码过期";
    case KG_LOGIN_ERROR:   return "登录失败";
    default: return "等待扫码";
    }
}

const char *kg_model_net_label(const kg_state_t *s)
{
    switch (s->net) {
    case KG_NET_OFFLINE:     return "演示曲库";
    case KG_NET_CONNECTING:  return "连接中";
    case KG_NET_ONLINE:      return "在线";
    case KG_NET_ERROR:       return "网络异常";
    default: return "演示曲库";
    }
}

kg_action_t kg_model_key(kg_state_t *s, const kg_catalog_t *c,
                         kg_key_t key, kg_key_ev_t ev)
{
    switch (s->page) {
    case KG_PAGE_LOGIN:
        if (key == KG_KEY_OK && ev == KG_KEY_CLICK) {
            if (s->login == KG_LOGIN_DONE) {
                s->page = KG_PAGE_PLAYLISTS;
                return KG_ACT_REPAINT;
            }
            if (s->net == KG_NET_OFFLINE) {
                // 未配置网络：本地演示闭环，明确标注而非假装真实登录
                s->demo_login = true;
                s->login = KG_LOGIN_DONE;
                kg_model_set_message(s, "演示模式 已跳过扫码");
                s->page = KG_PAGE_PLAYLISTS;
                return KG_ACT_REPAINT;
            }
            return KG_ACT_LOGIN_START;
        }
        if (key == KG_KEY_OK && ev == KG_KEY_LONG) return KG_ACT_LOGIN_REFRESH;
        // 长按下键进入网络设置，可换 Wi-Fi 或重新配网
        if (key == KG_KEY_DOWN && ev == KG_KEY_LONG) {
            s->page = KG_PAGE_WIFI;
            return KG_ACT_WIFI_SCAN;
        }
        return KG_ACT_NONE;

    case KG_PAGE_WIFI:
        if (ev == KG_KEY_CLICK) {
            if ((key == KG_KEY_UP || key == KG_KEY_DOWN) && s->ap_count > 0) {
                if (key == KG_KEY_UP) {
                    s->ap_cursor = (uint8_t)((s->ap_cursor + s->ap_count - 1) % s->ap_count);
                } else {
                    s->ap_cursor = (uint8_t)((s->ap_cursor + 1) % s->ap_count);
                }
                return KG_ACT_REPAINT;
            }
            if (key == KG_KEY_OK && s->ap_count > 0) {
                snprintf(s->ssid, sizeof(s->ssid), "%s",
                         s->aps[s->ap_cursor].ssid);
                s->password[0] = '\0';
                // 开放网络无需密码：直接发起连接，不再进键盘页
                if (s->aps[s->ap_cursor].open) {
                    return KG_ACT_WIFI_CONNECT;
                }
                s->page = KG_PAGE_KEYBOARD;
                return KG_ACT_REPAINT;
            }
        } else {
            if (key == KG_KEY_OK) return KG_ACT_WIFI_SCAN;
            if (key == KG_KEY_UP) { s->page = KG_PAGE_LOGIN; return KG_ACT_REPAINT; }
        }
        return KG_ACT_NONE;

    case KG_PAGE_KEYBOARD:
        // 键盘页按键（切组/删除/提交/取消）已统一在 main 的 handle_key 里
        // 交给 kg_keyboard_key 处理，这里不应再收到键盘页按键。
        return KG_ACT_NONE;

    case KG_PAGE_PLAYLISTS: {
        uint8_t n = c->playlist_count;
        if (n == 0) return KG_ACT_NONE;
        if (ev == KG_KEY_CLICK) {
            if (key == KG_KEY_UP) {
                s->playlist_cursor = (uint8_t)((s->playlist_cursor + n - 1) % n);
                return KG_ACT_REPAINT;
            }
            if (key == KG_KEY_DOWN) {
                if (s->playlist_cursor < n - 1) {
                    s->playlist_cursor = (uint8_t)(s->playlist_cursor + 1);
                    return KG_ACT_REPAINT;
                }
                // 已到已加载的最后一个歌单：在线且还有更多则翻页，否则回绕
                if (s->online_catalog && !s->playlists_done && n < KG_PLAYLIST_MAX) {
                    return KG_ACT_LOAD_MORE_PLAYLISTS;  // 光标停在末尾
                }
                s->playlist_cursor = 0;
                return KG_ACT_REPAINT;
            }
            // 确认：打开歌单，光标归零
            s->track_cursor = 0;
            s->batch_played = 0;
            s->pending_play = false;
            s->page = KG_PAGE_TRACKLIST;
            return KG_ACT_REPAINT;
        }
        if (key == KG_KEY_OK) {
            s->page = KG_PAGE_LOGIN;
            s->login = KG_LOGIN_WAIT;
            s->demo_login = false;
            s->playing = false;
            kg_model_set_message(s, "已退出登录");
            return KG_ACT_LOGOUT;
        }
        return KG_ACT_NONE;
    }

    case KG_PAGE_TRACKLIST: {
        uint8_t n = kg_list_size(c, s->playlist_cursor);
        if (ev == KG_KEY_CLICK) {
            if (n == 0) return KG_ACT_NONE;  // 空列表不响应点击，但仍允许长按 OK 返回
            if (key == KG_KEY_UP) {
                s->track_cursor = (uint8_t)((s->track_cursor + n - 1) % n);
                return KG_ACT_REPAINT;
            }
            if (key == KG_KEY_DOWN) {
                if (s->track_cursor < n - 1) {
                    s->track_cursor = (uint8_t)(s->track_cursor + 1);
                    return KG_ACT_REPAINT;
                }
                // 已到已加载的最后一首：在线且还有更多则翻页，否则回绕
                const kg_playlist_t *list = &c->playlists[s->playlist_cursor];
                if (s->online_catalog && list->count < list->total &&
                    list->count < KG_TRACKS_PER_LIST) {
                    return KG_ACT_LOAD_MORE_TRACKS;  // 光标停在末尾
                }
                s->track_cursor = 0;
                return KG_ACT_REPAINT;
            }
            load_at(s, c, s->track_cursor);
            s->playing = true;
            s->page = KG_PAGE_PLAYER;
            return KG_ACT_TRACK_CHANGED;
        }
        if (key == KG_KEY_OK) {
            s->page = KG_PAGE_PLAYLISTS;
            return KG_ACT_REPAINT;
        }
        return KG_ACT_NONE;
    }

    case KG_PAGE_PLAYER:
        if (ev == KG_KEY_CLICK) {
            if (key == KG_KEY_OK) {
                s->playing = !s->playing;
                return KG_ACT_PLAY_TOGGLE;
            }
            if (key == KG_KEY_UP) {
                advance(s, c, -1);
                return KG_ACT_TRACK_CHANGED;
            }
            if (key == KG_KEY_DOWN) {
                advance(s, c, 1);
                return KG_ACT_TRACK_CHANGED;
            }
        } else {
            if (key == KG_KEY_OK) {
                return KG_ACT_NONE;  // 已取消顺序/随机切换，长按确认无操作
            }
            if (key == KG_KEY_UP || key == KG_KEY_DOWN) {
                s->page = KG_PAGE_TRACKLIST;
                return KG_ACT_REPAINT;
            }
        }
        return KG_ACT_NONE;

    default:
        return KG_ACT_NONE;
    }
}

kg_action_t kg_model_tick(kg_state_t *s, const kg_catalog_t *c, uint32_t seconds)
{
    if (!s->playing || seconds == 0) return KG_ACT_NONE;
    const kg_track_t *track = kg_loaded_track(c, s);
    if (!track || track->duration_s == 0) return KG_ACT_NONE;

    s->elapsed_s += seconds;
    if (s->elapsed_s >= track->duration_s) {
        advance(s, c, 1);
        return KG_ACT_TRACK_CHANGED;
    }
    return KG_ACT_REPAINT;
}
