// tests/test_kugou_model.c —— 播放器状态机的主机测试（不依赖 ESP-IDF / LVGL）。
//
// 覆盖：内置演示曲库、登录态切换、页面导航、顺序播放、tick 自动推进，
// 以及在线歌单/曲目的分页翻页与"播完自动加载下一批"的光标行为。
#include <stdio.h>
#include <string.h>

#include "kugou_model.h"

static int failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static void enter_playlists(kg_state_t *s, const kg_catalog_t *c)
{
    kg_model_set_net(s, KG_NET_ONLINE);
    kg_model_set_login(s, KG_LOGIN_DONE);
    s->online_catalog = true;  // 在线曲库：翻页行为才生效
    CHECK(kg_model_key(s, c, KG_KEY_OK, KG_KEY_CLICK) == KG_ACT_REPAINT);
    CHECK(s->page == KG_PAGE_PLAYLISTS);
}

static void enter_player(kg_state_t *s, const kg_catalog_t *c)
{
    CHECK(kg_model_key(s, c, KG_KEY_OK, KG_KEY_CLICK) == KG_ACT_REPAINT);
    CHECK(s->page == KG_PAGE_TRACKLIST);
    CHECK(kg_model_key(s, c, KG_KEY_OK, KG_KEY_CLICK) == KG_ACT_TRACK_CHANGED);
    CHECK(s->page == KG_PAGE_PLAYER);
}

// 造一个"只加载了 4 首、总共 20 首"的在线歌单，用于分页断言。
static void make_online_catalog(kg_catalog_t *c)
{
    memset(c, 0, sizeof(*c));
    c->playlist_count = 1;
    kg_playlist_t *p = &c->playlists[0];
    snprintf(p->name, sizeof(p->name), "%s", "测试歌单");
    snprintf(p->collection_id, sizeof(p->collection_id), "%s", "12345");
    p->accent = 0x2CA8FF;
    p->first = 0;
    p->count = 4;
    p->total = 20;
    c->track_count = 4;
    for (int i = 0; i < 4; i++) {
        kg_track_t *t = &c->tracks[i];
        snprintf(t->title, sizeof(t->title), "曲目%d", i + 1);
        snprintf(t->artist, sizeof(t->artist), "歌手%d", i + 1);
        snprintf(t->hash, sizeof(t->hash), "hash%d", i);
        t->duration_s = 200;
        t->seed = (uint8_t)i;
    }
}

static void test_catalog(void)
{
    kg_catalog_t c;
    kg_catalog_default(&c);
    // 内置演示曲库：6 歌单 x 8 首，与在线分页容量（KG_PLAYLIST_MAX/KG_TRACKS_PER_LIST）无关
    CHECK(c.playlist_count == 6);
    CHECK(c.track_count == 48);
    for (uint8_t i = 0; i < c.playlist_count; i++) {
        CHECK(c.playlists[i].count == 8);
        CHECK(kg_list_size(&c, i) == 8);
        CHECK(kg_list_track(&c, i, 0) == (int)c.playlists[i].first);
        CHECK(kg_list_track(&c, i, 7) == (int)c.playlists[i].first + 7);
        CHECK(kg_list_track(&c, i, 8) == -1);
        CHECK(c.playlists[i].name[0] != '\0');
    }
    CHECK(kg_list_track(&c, 200, 0) == -1);
    CHECK(kg_list_size(&c, 200) == 0);
    CHECK(c.tracks[0].duration_s > 0);
    CHECK(strlen(c.tracks[0].title) > 0);
}

static void test_init_and_login(void)
{
    kg_catalog_t c;
    kg_state_t s;
    kg_catalog_default(&c);
    kg_model_init(&s, &c);

    CHECK(s.page == KG_PAGE_LOGIN);
    CHECK(s.net == KG_NET_OFFLINE);
    CHECK(s.login == KG_LOGIN_WAIT);
    CHECK(s.playing == false);
    CHECK(s.battery == -1);
    CHECK(s.current_track == kg_list_track(&c, 0, 0));
    CHECK(strcmp(kg_model_net_label(&s), "演示曲库") == 0);

    // 离线演示：按确认键明确进入演示模式，而不是伪装成真实登录
    CHECK(kg_model_key(&s, &c, KG_KEY_OK, KG_KEY_CLICK) == KG_ACT_REPAINT);
    CHECK(s.demo_login == true);
    CHECK(s.login == KG_LOGIN_DONE);
    CHECK(s.page == KG_PAGE_PLAYLISTS);
    CHECK(kg_model_key(&s, &c, KG_KEY_OK, KG_KEY_CLICK) == KG_ACT_REPAINT);
    CHECK(s.page == KG_PAGE_TRACKLIST);
    // 播放列表页长按确认返回歌单页
    CHECK(kg_model_key(&s, &c, KG_KEY_OK, KG_KEY_LONG) == KG_ACT_REPAINT);
    CHECK(s.page == KG_PAGE_PLAYLISTS);
    // 歌单页长按确认退出登录
    CHECK(kg_model_key(&s, &c, KG_KEY_OK, KG_KEY_LONG) == KG_ACT_LOGOUT);
    CHECK(s.page == KG_PAGE_LOGIN);
    CHECK(s.login == KG_LOGIN_WAIT);
    CHECK(s.demo_login == false);

    // 在线但未登录时，按确认键要发起轮询，不能直接放行
    kg_model_set_net(&s, KG_NET_ONLINE);
    CHECK(kg_model_key(&s, &c, KG_KEY_OK, KG_KEY_LONG) == KG_ACT_LOGIN_REFRESH);
    CHECK(kg_model_key(&s, &c, KG_KEY_OK, KG_KEY_CLICK) == KG_ACT_LOGIN_START);
    CHECK(s.page == KG_PAGE_LOGIN);
    kg_model_set_login(&s, KG_LOGIN_DONE);
    CHECK(kg_model_key(&s, &c, KG_KEY_OK, KG_KEY_CLICK) == KG_ACT_REPAINT);
    CHECK(s.page == KG_PAGE_PLAYLISTS);
}

static void test_navigation_and_playback(void)
{
    kg_catalog_t c;
    kg_state_t s;
    kg_catalog_default(&c);
    kg_model_init(&s, &c);
    enter_player(&s, &c);

    CHECK(s.playing == true);
    CHECK(s.current_track == kg_list_track(&c, 0, 0));
    CHECK(kg_loaded_track(&c, &s) != NULL);

    CHECK(kg_model_key(&s, &c, KG_KEY_OK, KG_KEY_CLICK) == KG_ACT_PLAY_TOGGLE);
    CHECK(s.playing == false);
    CHECK(kg_model_key(&s, &c, KG_KEY_OK, KG_KEY_CLICK) == KG_ACT_PLAY_TOGGLE);
    CHECK(s.playing == true);

    CHECK(kg_model_key(&s, &c, KG_KEY_DOWN, KG_KEY_CLICK) == KG_ACT_TRACK_CHANGED);
    CHECK(s.track_cursor == 1);
    CHECK(kg_model_key(&s, &c, KG_KEY_UP, KG_KEY_CLICK) == KG_ACT_TRACK_CHANGED);
    CHECK(s.track_cursor == 0);
    // 顺序模式从第一首往回退，循环到最后一首
    CHECK(kg_model_key(&s, &c, KG_KEY_UP, KG_KEY_CLICK) == KG_ACT_TRACK_CHANGED);
    CHECK(s.track_cursor == 7);
    CHECK(kg_model_key(&s, &c, KG_KEY_DOWN, KG_KEY_CLICK) == KG_ACT_TRACK_CHANGED);
    CHECK(s.track_cursor == 0);
    CHECK(s.elapsed_s == 0);

    // 详情页长按上/下返回播放列表
    CHECK(kg_model_key(&s, &c, KG_KEY_DOWN, KG_KEY_LONG) == KG_ACT_REPAINT);
    CHECK(s.page == KG_PAGE_TRACKLIST);
    CHECK(kg_model_key(&s, &c, KG_KEY_OK, KG_KEY_CLICK) == KG_ACT_TRACK_CHANGED);
    CHECK(kg_model_key(&s, &c, KG_KEY_UP, KG_KEY_LONG) == KG_ACT_REPAINT);
    CHECK(s.page == KG_PAGE_TRACKLIST);
}

static void test_playlist_wrap(void)
{
    kg_catalog_t c;
    kg_state_t s;
    kg_catalog_default(&c);
    kg_model_init(&s, &c);
    CHECK(kg_model_key(&s, &c, KG_KEY_OK, KG_KEY_CLICK) == KG_ACT_REPAINT);

    CHECK(kg_model_key(&s, &c, KG_KEY_UP, KG_KEY_CLICK) == KG_ACT_REPAINT);
    CHECK(s.playlist_cursor == c.playlist_count - 1);
    CHECK(kg_model_key(&s, &c, KG_KEY_DOWN, KG_KEY_CLICK) == KG_ACT_REPAINT);
    CHECK(s.playlist_cursor == 0);

    CHECK(kg_model_key(&s, &c, KG_KEY_OK, KG_KEY_CLICK) == KG_ACT_REPAINT);
    for (int i = 0; i < 8; i++) {
        CHECK(kg_model_key(&s, &c, KG_KEY_DOWN, KG_KEY_CLICK) == KG_ACT_REPAINT);
    }
    CHECK(s.track_cursor == 0);  // 曲目光标同样循环

    CHECK(kg_model_key(&s, &c, KG_KEY_OK, KG_KEY_CLICK) == KG_ACT_TRACK_CHANGED);
    CHECK(strcmp(kg_model_mode_label(&s), "顺序播放") == 0);
    // 已取消随机/顺序切换：详情页长按确认不再切换模式
    CHECK(kg_model_key(&s, &c, KG_KEY_OK, KG_KEY_LONG) == KG_ACT_NONE);
    CHECK(strcmp(kg_model_mode_label(&s), "顺序播放") == 0);
}

static void test_online_track_pagination(void)
{
    kg_catalog_t c;
    kg_state_t s;
    make_online_catalog(&c);
    kg_model_init(&s, &c);
    enter_playlists(&s, &c);
    CHECK(kg_model_key(&s, &c, KG_KEY_OK, KG_KEY_CLICK) == KG_ACT_REPAINT);
    CHECK(s.page == KG_PAGE_TRACKLIST);
    CHECK(s.track_cursor == 0);

    // 移到已加载的最后一首（第 4 首，cursor=3）
    for (int i = 0; i < 3; i++) {
        CHECK(kg_model_key(&s, &c, KG_KEY_DOWN, KG_KEY_CLICK) == KG_ACT_REPAINT);
    }
    CHECK(s.track_cursor == 3);
    // 再按下：应请求加载下一批，而不是回绕到 0
    CHECK(kg_model_key(&s, &c, KG_KEY_DOWN, KG_KEY_CLICK) == KG_ACT_LOAD_MORE_TRACKS);
    CHECK(s.track_cursor == 3);

    // 模拟下一批到达：count 4 -> 8
    c.playlists[0].count = 8;
    c.track_count = 8;
    // 现在能继续向下移动到第 5 首
    CHECK(kg_model_key(&s, &c, KG_KEY_DOWN, KG_KEY_CLICK) == KG_ACT_REPAINT);
    CHECK(s.track_cursor == 4);
}

static void test_online_playlist_pagination(void)
{
    kg_catalog_t c;
    kg_state_t s;
    memset(&c, 0, sizeof(c));
    c.playlist_count = 4;
    for (int i = 0; i < 4; i++) {
        snprintf(c.playlists[i].name, sizeof(c.playlists[i].name), "歌单%d", i + 1);
        snprintf(c.playlists[i].collection_id, sizeof(c.playlists[i].collection_id), "%d", i + 1);
    }
    kg_model_init(&s, &c);
    enter_playlists(&s, &c);

    for (int i = 0; i < 3; i++) {
        CHECK(kg_model_key(&s, &c, KG_KEY_DOWN, KG_KEY_CLICK) == KG_ACT_REPAINT);
    }
    CHECK(s.playlist_cursor == 3);
    // 已到已加载末尾且未到底：请求加载下一批
    CHECK(kg_model_key(&s, &c, KG_KEY_DOWN, KG_KEY_CLICK) == KG_ACT_LOAD_MORE_PLAYLISTS);
    CHECK(s.playlist_cursor == 3);
    // 标记歌单已到底后，再按向下回绕
    s.playlists_done = true;
    CHECK(kg_model_key(&s, &c, KG_KEY_DOWN, KG_KEY_CLICK) == KG_ACT_REPAINT);
    CHECK(s.playlist_cursor == 0);
}

static void test_tick_autoadvance(void)
{
    kg_catalog_t c;
    kg_state_t s;
    kg_catalog_default(&c);
    kg_model_init(&s, &c);
    enter_player(&s, &c);

    const kg_track_t *t = kg_loaded_track(&c, &s);
    CHECK(t != NULL);
    CHECK(kg_model_tick(&s, &c, 1) == KG_ACT_REPAINT);
    CHECK(s.elapsed_s == 1);
    CHECK(kg_model_tick(&s, &c, (uint32_t)t->duration_s) == KG_ACT_TRACK_CHANGED);
    CHECK(s.elapsed_s == 0);
    CHECK(s.track_cursor == 1);

    s.playing = false;
    CHECK(kg_model_tick(&s, &c, 10) == KG_ACT_NONE);
    CHECK(s.elapsed_s == 0);
}

static void test_setters(void)
{
    kg_catalog_t c;
    kg_state_t s;
    kg_catalog_default(&c);
    kg_model_init(&s, &c);
    kg_model_set_battery(&s, 77);
    CHECK(s.battery == 77);
    kg_model_set_net(&s, KG_NET_ERROR);
    CHECK(strcmp(kg_model_net_label(&s), "网络异常") == 0);
    kg_model_set_login(&s, KG_LOGIN_SCANNED);
    CHECK(strcmp(kg_model_login_label(&s), "等待确认") == 0);
    kg_model_set_message(&s, "测试消息");
    CHECK(strcmp(s.message, "测试消息") == 0);
}

int main(void)
{
    test_catalog();
    test_init_and_login();
    test_navigation_and_playback();
    test_playlist_wrap();
    test_online_track_pagination();
    test_online_playlist_pagination();
    test_tick_autoadvance();
    test_setters();
    if (failures) {
        printf("kugou model: FAIL (%d)\n", failures);
        return 1;
    }
    printf("kugou model: PASS\n");
    return 0;
}
