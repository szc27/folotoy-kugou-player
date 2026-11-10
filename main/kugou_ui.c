// main/kugou_ui.c —— 见 kugou_ui.h。
//
// 屏幕 240x320，四角有 30px 圆角遮罩，内容限制在 x=12..228、y=24..300。
// 页面切换时重建一次，同一页面内只更新变化的文本、颜色与进度，避免每秒
// 整屏重绘导致的闪烁。所有 LVGL 访问都在 bsp_lvgl_lock() 之内。
#include "kugou_ui.h"

#include <stdio.h>
#include <string.h>

#include "app_fonts.h"
#include "bsp_display.h"
#include "kugou_keyboard.h"
#include "lvgl.h"

/* 前向声明：被 update_wifi 等前置调用，定义在文件后部 */
static int window_first(int cursor, int rows, int total);
static void update_common(const kg_state_t *s, const char *title);

#define C_BG       0x0B1220
#define C_SURFACE  0x16213A
#define C_SURF_HI  0x223A63
#define C_ACCENT   0x2CA8FF
#define C_PINK     0xFF5D8F
#define C_TEXT     0xFFFFFF
#define C_MUTED    0x8A97B5
#define C_LINE     0x25314E
#define C_WHITE    0xFFFFFF

#define SCREEN_W   240
#define PAD_X      12
#define CONTENT_W  216
#define LIST_ROWS  5

typedef struct {
    kg_page_t page;
    bool built;
    char qr_url[KG_QR_URL_CAP];
    bool qr_dirty;
    lv_obj_t *qr;

    lv_obj_t *lbl_header;
    lv_obj_t *lbl_net;
    lv_obj_t *lbl_battery;
    lv_obj_t *lbl_message;
    lv_obj_t *lbl_hint;

    lv_obj_t *row[LIST_ROWS];
    lv_obj_t *row_index[LIST_ROWS];
    lv_obj_t *row_title[LIST_ROWS];
    lv_obj_t *row_sub[LIST_ROWS];
    lv_obj_t *row_mark[LIST_ROWS];

    lv_obj_t *lbl_song;
    lv_obj_t *lbl_artist;
    lv_obj_t *lbl_mode;
    lv_obj_t *lbl_time_cur;
    lv_obj_t *lbl_time_total;
    lv_obj_t *bar_fill;
    lv_obj_t *cover_ring;
    lv_obj_t *cover_core;
    lv_obj_t *btn_play_label;
    lv_obj_t *lbl_kb_char;
    lv_obj_t *lbl_kb_text;
    lv_obj_t *lbl_kb_group;
    lv_obj_t *lbl_ssid;
} ui_t;

static ui_t s_ui;

// ---------------------------------------------------------------------------
// 基础控件
// ---------------------------------------------------------------------------

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, uint32_t color,
                            int x, int y, int w, lv_text_align_t align)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int w, int h,
                          uint32_t color, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    return o;
}

static void set_text(lv_obj_t *label, const char *text)
{
    if (!label || !text) return;
    const char *current = lv_label_get_text(label);
    if (current && strcmp(current, text) == 0) return;
    lv_label_set_text(label, text);
}

static void set_bg(lv_obj_t *obj, uint32_t color)
{
    if (obj) lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
}

static void set_fg(lv_obj_t *obj, uint32_t color)
{
    if (obj) lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
}

static void draw_chrome(lv_obj_t *scr, const char *hint)
{
    s_ui.lbl_header = make_label(scr, "", app_font_title(), C_TEXT,
                                 PAD_X, 24, 140, LV_TEXT_ALIGN_LEFT);
    s_ui.lbl_net = make_label(scr, "", app_font_body(), C_MUTED, 148, 28, 42,
                              LV_TEXT_ALIGN_RIGHT);
    s_ui.lbl_battery = make_label(scr, "", app_font_body(), C_MUTED, 192, 28, 34,
                                  LV_TEXT_ALIGN_RIGHT);
    make_box(scr, PAD_X, 50, CONTENT_W, 1, C_LINE, 0);
    s_ui.lbl_hint = make_label(scr, hint, app_font_body(), C_MUTED,
                               PAD_X, 286, CONTENT_W, LV_TEXT_ALIGN_CENTER);
    make_box(scr, PAD_X, 280, CONTENT_W, 1, C_LINE, 0);
}

// ---------------------------------------------------------------------------
// 扫码登录页
// ---------------------------------------------------------------------------

static void build_login(lv_obj_t *scr)
{
    make_label(scr, "打开酷狗音乐 扫一扫", app_font_body(), C_MUTED,
               PAD_X, 62, CONTENT_W, LV_TEXT_ALIGN_CENTER);
    // 白色静区，二维码在中间
    make_box(scr, 62, 88, 116, 116, C_WHITE, 8);
    lv_obj_t *qr = lv_qrcode_create(scr);
    lv_qrcode_set_size(qr, 104);
    lv_qrcode_set_dark_color(qr, lv_color_hex(C_BG));
    lv_qrcode_set_light_color(qr, lv_color_hex(C_WHITE));
    // 白底面板本身就是静区，不再让 QR 内部占用模块，长 URL 也能扫得动
    lv_qrcode_set_quiet_zone(qr, false);
    lv_obj_set_pos(qr, 68, 94);
    s_ui.qr = qr;
    s_ui.qr_dirty = true;

    make_label(scr, "登录状态", app_font_body(), C_MUTED,
               PAD_X, 214, CONTENT_W, LV_TEXT_ALIGN_CENTER);
    s_ui.lbl_song = make_label(scr, "", app_font_title(), C_ACCENT,
                               PAD_X, 234, CONTENT_W, LV_TEXT_ALIGN_CENTER);
    s_ui.lbl_message = make_label(scr, "", app_font_body(), C_MUTED,
                                  PAD_X, 258, CONTENT_W, LV_TEXT_ALIGN_CENTER);
    draw_chrome(scr, "进入  长按刷新  长按下换网");
}

// ---------------------------------------------------------------------------
// 列表页（歌单与播放列表共用骨架，行内容与配色不同）
// ---------------------------------------------------------------------------

static void build_list(lv_obj_t *scr, bool is_playlist)
{
    int rows = is_playlist ? 4 : LIST_ROWS;
    int top = 78;
    int step = is_playlist ? 48 : 38;
    int height = is_playlist ? 46 : 34;

    s_ui.lbl_song = make_label(scr, "", app_font_body(), C_MUTED,
                               PAD_X, 58, CONTENT_W, LV_TEXT_ALIGN_LEFT);

    for (int i = 0; i < LIST_ROWS; i++) {
        if (i >= rows) {
            s_ui.row[i] = NULL;
            s_ui.row_index[i] = NULL;
            s_ui.row_title[i] = NULL;
            s_ui.row_sub[i] = NULL;
            s_ui.row_mark[i] = NULL;
            continue;
        }
        lv_obj_t *card = make_box(scr, PAD_X, top + i * step, CONTENT_W, height,
                                  C_SURFACE, 10);
        s_ui.row[i] = card;
        s_ui.row_mark[i] = make_box(card, 0, 8, 3, height - 16, C_ACCENT, 2);
        s_ui.row_index[i] = make_label(card, "", app_font_body(), C_MUTED,
                                       10, height / 2 - 10, 26, LV_TEXT_ALIGN_LEFT);
        s_ui.row_title[i] = make_label(card, "", app_font_title(), C_TEXT,
                                       38, is_playlist ? 3 : 6, 150,
                                       LV_TEXT_ALIGN_LEFT);
        s_ui.row_sub[i] = make_label(card, "", app_font_body(), C_MUTED,
                                     38, is_playlist ? 26 : 7, 150,
                                     LV_TEXT_ALIGN_RIGHT);
        if (is_playlist) {
            // 歌单页：副标题在左下；曲目页：右侧显示时长
            lv_obj_set_pos(s_ui.row_sub[i], 38, 26);
            lv_obj_set_style_text_align(s_ui.row_sub[i], LV_TEXT_ALIGN_LEFT, 0);
        } else {
            // 曲目页：歌名左、时长右，两者不能叠在一起
            lv_obj_set_width(s_ui.row_title[i], 110);
            lv_obj_set_width(s_ui.row_sub[i], 50);
            lv_obj_set_pos(s_ui.row_sub[i], 154, 7);
            lv_obj_set_style_text_font(s_ui.row_sub[i], app_font_body(), 0);
        }
    }
    // 列表页不额外放消息行：4/5 行卡片已排到 y=268/264，再叠一行会与底栏相撞。
    // set_text() 对 NULL 标签是安全的，状态消息改由登录页与播放页呈现。
    s_ui.lbl_message = NULL;
    draw_chrome(scr, is_playlist ? "确认打开  长按退出登录" : "确认播放  长按返回");
}

// ---------------------------------------------------------------------------
// 配网：AP 列表与三键屏幕键盘
// ---------------------------------------------------------------------------

static void build_wifi(lv_obj_t *scr)
{
    s_ui.lbl_song = make_label(scr, "", app_font_body(), C_MUTED,
                               PAD_X, 58, CONTENT_W, LV_TEXT_ALIGN_LEFT);
    for (int i = 0; i < LIST_ROWS; i++) {
        s_ui.row[i] = NULL;
        s_ui.row_index[i] = NULL;
        s_ui.row_title[i] = NULL;
        s_ui.row_sub[i] = NULL;
        s_ui.row_mark[i] = NULL;
    }
    for (int i = 0; i < 4; i++) {
        lv_obj_t *card = make_box(scr, PAD_X, 80 + i * 44, CONTENT_W, 40,
                                  C_SURFACE, 10);
        s_ui.row[i] = card;
        s_ui.row_mark[i] = make_box(card, 0, 7, 3, 26, C_ACCENT, 2);
        s_ui.row_index[i] = make_label(card, "", app_font_body(), C_MUTED,
                                       10, 10, 22, LV_TEXT_ALIGN_LEFT);
        s_ui.row_title[i] = make_label(card, "", app_font_body(), C_TEXT,
                                       34, 4, 128, LV_TEXT_ALIGN_LEFT);
        s_ui.row_sub[i] = make_label(card, "", app_font_body(), C_MUTED,
                                     34, 22, 168, LV_TEXT_ALIGN_LEFT);
    }
    s_ui.lbl_message = make_label(scr, "", app_font_body(), C_MUTED,
                                  PAD_X, 260, CONTENT_W, LV_TEXT_ALIGN_CENTER);
    draw_chrome(scr, "上下选择  确认连接  长按刷新");
}

static void build_keyboard(lv_obj_t *scr)
{
    s_ui.lbl_ssid = make_label(scr, "", app_font_body(), C_MUTED,
                               PAD_X, 58, CONTENT_W, LV_TEXT_ALIGN_LEFT);
    s_ui.lbl_kb_group = make_label(scr, "", app_font_body(), C_ACCENT,
                                   PAD_X, 80, CONTENT_W, LV_TEXT_ALIGN_LEFT);
    make_box(scr, 88, 104, 64, 64, C_SURFACE, 12);
    s_ui.lbl_kb_char = make_label(scr, "", app_font_title(), C_TEXT,
                                  88, 122, 64, LV_TEXT_ALIGN_CENTER);
    s_ui.lbl_kb_text = make_label(scr, "", app_font_body(), C_ACCENT,
                                  PAD_X, 182, CONTENT_W, LV_TEXT_ALIGN_CENTER);
    s_ui.lbl_message = make_label(scr, "", app_font_body(), C_MUTED,
                                  PAD_X, 212, CONTENT_W, LV_TEXT_ALIGN_CENTER);
    make_label(scr, "上下选字符  确认输入", app_font_body(), C_MUTED,
               PAD_X, 240, CONTENT_W, LV_TEXT_ALIGN_CENTER);
    make_label(scr, "长按上换组  长按下删除", app_font_body(), C_MUTED,
               PAD_X, 260, CONTENT_W, LV_TEXT_ALIGN_CENTER);
    draw_chrome(scr, "长按确认连接  空密码返回");
}

static void update_wifi(const kg_state_t *s)
{
    char subtitle[32];
    snprintf(subtitle, sizeof(subtitle), "共 %d 个网络", (int)s->ap_count);
    update_common(s, "选择网络");
    set_text(s_ui.lbl_song, subtitle);
    set_text(s_ui.lbl_message, s->message);

    int first = window_first((int)s->ap_cursor, 4, (int)s->ap_count);
    char idx_text[12], rssi_text[48];
    for (int i = 0; i < 4; i++) {
        int idx = first + i;
        lv_obj_t *card = s_ui.row[i];
        if (!card) continue;
        if (idx >= (int)s->ap_count) {
            lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(card, LV_OBJ_FLAG_HIDDEN);
        bool selected = (idx == (int)s->ap_cursor);
        set_bg(card, selected ? C_SURF_HI : C_SURFACE);
        set_bg(s_ui.row_mark[i], selected ? C_ACCENT : C_LINE);
        snprintf(idx_text, sizeof(idx_text), "%02d", idx + 1);
        set_text(s_ui.row_index[i], idx_text);
        set_text(s_ui.row_title[i], s->aps[idx].ssid);
        snprintf(rssi_text, sizeof(rssi_text), "%s  信号 %d",
                 s->aps[idx].open ? "开放网络" : "需要密码",
                 (int)s->aps[idx].rssi);
        set_text(s_ui.row_sub[i], rssi_text);
        set_fg(s_ui.row_title[i], selected ? C_ACCENT : C_TEXT);
    }
}

static void update_keyboard(const kg_state_t *s)
{
    update_common(s, "输入密码");
    char ssid_text[64], group_text[48], char_text[8], masked[KG_PASS_CAP];
    snprintf(ssid_text, sizeof(ssid_text), "网络 %s", s->ssid);
    snprintf(group_text, sizeof(group_text), "字符组 %s",
             kg_keyboard_group_name(s->kb_group));
    kg_keyboard_t kb;
    kg_keyboard_init(&kb);
    kb.group = s->kb_group;
    kb.index = s->kb_index;
    char current = kg_keyboard_current(&kb);
    snprintf(char_text, sizeof(char_text), "%c", current == ' ' ? '_' : current);
    size_t n = strlen(s->password);
    for (size_t i = 0; i < n && i + 1 < sizeof(masked); i++) masked[i] = '*';
    masked[n < sizeof(masked) ? n : sizeof(masked) - 1] = '\0';
    set_text(s_ui.lbl_ssid, ssid_text);
    set_text(s_ui.lbl_kb_group, group_text);
    set_text(s_ui.lbl_kb_char, char_text);
    set_text(s_ui.lbl_kb_text, masked);
    set_text(s_ui.lbl_message, s->message);
}

// ---------------------------------------------------------------------------
// 播放详情页
// ---------------------------------------------------------------------------

static void build_player(lv_obj_t *scr)
{
    s_ui.lbl_song = make_label(scr, "", app_font_body(), C_MUTED,
                               PAD_X, 56, 130, LV_TEXT_ALIGN_LEFT);
    s_ui.lbl_mode = make_label(scr, "", app_font_body(), C_ACCENT,
                               148, 56, 80, LV_TEXT_ALIGN_RIGHT);

    // 唱片：外圈 + 高光环 + 中心轴，全部用普通 LVGL 对象绘制
    make_box(scr, 66, 70, 108, 108, C_SURFACE, 54);
    lv_obj_t *ring = make_box(scr, 76, 80, 88, 88, C_BG, 44);
    s_ui.cover_ring = make_box(ring, 10, 10, 68, 68, C_SURF_HI, 34);
    s_ui.cover_core = make_box(s_ui.cover_ring, 24, 24, 20, 20, C_ACCENT, 10);

    s_ui.lbl_artist = make_label(scr, "", app_font_title(), C_TEXT,
                                 PAD_X, 184, CONTENT_W, LV_TEXT_ALIGN_CENTER);
    s_ui.lbl_message = make_label(scr, "", app_font_body(), C_MUTED,
                                  PAD_X, 208, CONTENT_W, LV_TEXT_ALIGN_CENTER);

    make_box(scr, PAD_X, 230, CONTENT_W, 4, C_LINE, 2);
    s_ui.bar_fill = make_box(scr, PAD_X, 230, 0, 4, C_ACCENT, 2);
    s_ui.lbl_time_cur = make_label(scr, "00:00", app_font_body(), C_MUTED,
                                   PAD_X, 238, 70, LV_TEXT_ALIGN_LEFT);
    s_ui.lbl_time_total = make_label(scr, "00:00", app_font_body(), C_MUTED,
                                     158, 238, 70, LV_TEXT_ALIGN_RIGHT);

    const int btn_y = 252;
    lv_obj_t *prev = make_box(scr, 26, btn_y, 60, 28, C_SURFACE, 14);
    make_label(prev, LV_SYMBOL_PREV, &lv_font_montserrat_14, C_TEXT,
               0, 5, 60, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *play = make_box(scr, 90, btn_y, 60, 28, C_ACCENT, 14);
    s_ui.btn_play_label = make_label(play, LV_SYMBOL_PLAY,
                                     &lv_font_montserrat_14, C_BG,
                                     0, 5, 60, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *next = make_box(scr, 154, btn_y, 60, 28, C_SURFACE, 14);
    make_label(next, LV_SYMBOL_NEXT, &lv_font_montserrat_14, C_TEXT,
               0, 5, 60, LV_TEXT_ALIGN_CENTER);

    draw_chrome(scr, "确认暂停  长按换模式");
}

// ---------------------------------------------------------------------------
// 每帧更新
// ---------------------------------------------------------------------------

static void fmt_time(char *buf, size_t cap, uint32_t seconds)
{
    snprintf(buf, cap, "%02u:%02u", (unsigned)(seconds / 60) % 100u,
             (unsigned)seconds % 60u);
}

static int window_first(int cursor, int rows, int total)
{
    if (total <= rows) return 0;
    int first = cursor - rows / 2;
    if (first < 0) first = 0;
    if (first > total - rows) first = total - rows;
    return first;
}

static void update_common(const kg_state_t *s, const char *title)
{
    set_text(s_ui.lbl_header, title);
    set_text(s_ui.lbl_net, kg_model_net_label(s));
    char battery[16];
    if (s->battery >= 0) snprintf(battery, sizeof(battery), "%d%%", s->battery);
    else snprintf(battery, sizeof(battery), "--");
    set_text(s_ui.lbl_battery, battery);
}

static void update_login(const kg_state_t *s)
{
    update_common(s, "扫码登录");
    set_text(s_ui.lbl_song, kg_model_login_label(s));
    set_text(s_ui.lbl_message, s->message);
    if (s_ui.qr && s_ui.qr_dirty && s_ui.qr_url[0]) {
        (void)lv_qrcode_update(s_ui.qr, s_ui.qr_url,
                               (uint32_t)strlen(s_ui.qr_url));
        s_ui.qr_dirty = false;
    }
}

static void update_playlists(const kg_state_t *s, const kg_catalog_t *c)
{
    char subtitle[32];
    snprintf(subtitle, sizeof(subtitle), "共 %d 个歌单", (int)c->playlist_count);
    update_common(s, "我的歌单");
    set_text(s_ui.lbl_song, subtitle);
    set_text(s_ui.lbl_message, s->message);

    int total = c->playlist_count;
    int first = window_first((int)s->playlist_cursor, 4, total);
    char index_text[12];
    char sub_text[64];  // subtitle 最长 39 字节 + 分隔符 + 曲目数 + NUL
    for (int i = 0; i < 4; i++) {
        int idx = first + i;
        lv_obj_t *card = s_ui.row[i];
        if (!card) continue;
        bool visible = idx < total;
        if (!visible) {
            lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(card, LV_OBJ_FLAG_HIDDEN);
        bool selected = (idx == (int)s->playlist_cursor);
        set_bg(card, selected ? C_SURF_HI : C_SURFACE);
        set_bg(s_ui.row_mark[i], c->playlists[idx].accent);
        snprintf(index_text, sizeof(index_text), "%02d", idx + 1);
        set_text(s_ui.row_index[i], index_text);
        set_text(s_ui.row_title[i], c->playlists[idx].name);
        snprintf(sub_text, sizeof(sub_text), "%s  %d 首",
                 c->playlists[idx].subtitle, (int)c->playlists[idx].count);
        set_text(s_ui.row_sub[i], sub_text);
        set_fg(s_ui.row_title[i], selected ? C_ACCENT : C_TEXT);
    }
}

static void update_tracklist(const kg_state_t *s, const kg_catalog_t *c)
{
    uint8_t pl = s->playlist_cursor;
    if (pl >= c->playlist_count) pl = 0;
    int total = kg_list_size(c, pl);
    char title[KG_NAME_CAP];
    snprintf(title, sizeof(title), "%s", c->playlists[pl].name);
    update_common(s, title);
    char subtitle[32];
    snprintf(subtitle, sizeof(subtitle), "共 %d 首", total);
    set_text(s_ui.lbl_song, subtitle);
    set_text(s_ui.lbl_message, s->message);

    int first = window_first((int)s->track_cursor, LIST_ROWS, total);
    char index_text[12];
    char duration[16];
    for (int i = 0; i < LIST_ROWS; i++) {
        int idx = first + i;
        lv_obj_t *card = s_ui.row[i];
        if (!card) continue;
        if (idx >= total) {
            lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(card, LV_OBJ_FLAG_HIDDEN);
        int global = kg_list_track(c, pl, (uint8_t)idx);
        bool selected = (idx == (int)s->track_cursor);
        bool loaded = (global >= 0 && global == s->current_track);
        set_bg(card, selected ? C_SURF_HI : C_SURFACE);
        set_bg(s_ui.row_mark[i], loaded ? C_PINK : C_ACCENT);
        snprintf(index_text, sizeof(index_text), "%02d", idx + 1);
        set_text(s_ui.row_index[i], index_text);
        set_text(s_ui.row_title[i], global >= 0 ? c->tracks[global].title : "");
        if (global >= 0) fmt_time(duration, sizeof(duration),
                                  c->tracks[global].duration_s);
        else snprintf(duration, sizeof(duration), "00:00");
        set_text(s_ui.row_sub[i], duration);
        set_fg(s_ui.row_title[i], loaded ? C_PINK : (selected ? C_ACCENT : C_TEXT));
    }
}

static void update_player(const kg_state_t *s, const kg_catalog_t *c)
{
    update_common(s, "正在播放");
    set_text(s_ui.lbl_mode, kg_model_mode_label(s));

    uint8_t pl = s->playlist_cursor;
    if (pl >= c->playlist_count) pl = 0;
    const kg_track_t *t = kg_loaded_track(c, s);
    set_text(s_ui.lbl_song, c->playlists[pl].name);
    set_text(s_ui.lbl_artist, t ? t->title : "暂无歌曲");
    set_text(s_ui.lbl_message, t ? t->artist : s->message);
    set_bg(s_ui.cover_core, c->playlists[pl].accent);
    set_bg(s_ui.cover_ring, s->playing ? C_SURF_HI : C_SURFACE);
    set_text(s_ui.btn_play_label, s->playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    uint32_t duration = (t && t->duration_s) ? t->duration_s : 1;
    uint32_t elapsed = s->elapsed_s;
    if (elapsed > duration) elapsed = duration;
    uint32_t width = (uint32_t)((uint64_t)CONTENT_W * elapsed / duration);
    lv_obj_set_width(s_ui.bar_fill, (int32_t)width);
    char now[16];
    char total[16];
    fmt_time(now, sizeof(now), elapsed);
    fmt_time(total, sizeof(total), duration);
    set_text(s_ui.lbl_time_cur, now);
    set_text(s_ui.lbl_time_total, total);
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

void kg_ui_init(void)
{
    if (!bsp_lvgl_lock(1000)) return;
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.page = (kg_page_t)-1;
    bsp_lvgl_unlock();
}

void kg_ui_set_qr(const char *url)
{
    if (!url || !bsp_lvgl_lock(1000)) return;
    if (strcmp(s_ui.qr_url, url) != 0) {
        snprintf(s_ui.qr_url, sizeof(s_ui.qr_url), "%s", url);
        s_ui.qr_dirty = true;
    }
    bsp_lvgl_unlock();
}

void kg_ui_render(const kg_state_t *state, const kg_catalog_t *catalog)
{
    if (!state || !catalog || !bsp_lvgl_lock(1000)) return;
    lv_obj_t *scr = lv_screen_active();

    if (s_ui.page != state->page || !s_ui.built) {
        // 二维码 URL 由网络层异步送来，跨页面保留，避免切页后二维码空白
        char saved_qr[KG_QR_URL_CAP];
        snprintf(saved_qr, sizeof(saved_qr), "%s", s_ui.qr_url);
        lv_obj_clean(scr);
        lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        memset(&s_ui, 0, sizeof(s_ui));
        snprintf(s_ui.qr_url, sizeof(s_ui.qr_url), "%s", saved_qr);
        s_ui.qr_dirty = true;
        switch (state->page) {
        case KG_PAGE_LOGIN:     build_login(scr); break;
        case KG_PAGE_WIFI:      build_wifi(scr); break;
        case KG_PAGE_KEYBOARD:  build_keyboard(scr); break;
        case KG_PAGE_PLAYLISTS: build_list(scr, true); break;
        case KG_PAGE_TRACKLIST: build_list(scr, false); break;
        case KG_PAGE_PLAYER:    build_player(scr); break;
        default:                build_login(scr); break;
        }
        s_ui.page = state->page;
        s_ui.built = true;
    }

    switch (state->page) {
    case KG_PAGE_LOGIN:     update_login(state); break;
    case KG_PAGE_WIFI:      update_wifi(state); break;
    case KG_PAGE_KEYBOARD:  update_keyboard(state); break;
    case KG_PAGE_PLAYLISTS: update_playlists(state, catalog); break;
    case KG_PAGE_TRACKLIST: update_tracklist(state, catalog); break;
    case KG_PAGE_PLAYER:    update_player(state, catalog); break;
    default: break;
    }
    bsp_lvgl_unlock();
}
