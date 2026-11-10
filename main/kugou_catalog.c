// main/kugou_catalog.c —— 内置演示曲库。
//
// 未配置在线接口时，界面与播放逻辑仍然完整可用。曲目全部为演示用的虚构
// 名称，不附带任何真实录音，播放时的旋律由 kugou_audio.c 合成。
#include "kugou_model.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *title;
    const char *artist;
    uint16_t duration_s;
} track_def_t;

typedef struct {
    const char *name;
    const char *subtitle;
    uint32_t accent;
} list_def_t;

// 演示曲库固定 6 歌单 x 8 首，与在线分页容量（KG_PLAYLIST_MAX/KG_TRACK_MAX）无关。
#define DEMO_PLAYLISTS        6
#define DEMO_TRACKS_PER_LIST  8
#define DEMO_TRACKS           (DEMO_PLAYLISTS * DEMO_TRACKS_PER_LIST)

static const list_def_t k_lists[DEMO_PLAYLISTS] = {
    { "华语流行", "热门华语单曲", 0x2CA8FF },
    { "每日推荐", "今天听点好的", 0x8B72FF },
    { "经典老歌", "时光里的旋律", 0xFF9B4A },
    { "轻音乐",   "安静一会儿",   0x35C6A8 },
    { "电子节拍", "把音量调大",   0xFF5D8F },
    { "深夜电台", "一个人的夜晚", 0x5B8DEF },
};

// 6 个歌单 x 8 首，按歌单顺序连续存放。
static const track_def_t k_tracks[DEMO_TRACKS] = {
    // 华语流行
    { "晚风来信",     "林间",     212 },
    { "山海之间",     "陈一诺",   198 },
    { "蓝色黄昏",     "南山乐队", 235 },
    { "雨后的街",     "小舟",     186 },
    { "第七个夏天",   "白昼乐队", 224 },
    { "月光邮差",     "顾言",     203 },
    { "向北的列车",   "木子",     241 },
    { "旧照片",       "午后电台", 176 },
    // 每日推荐
    { "清晨六点",     "林间",     208 },
    { "云上散步",     "小舟",     192 },
    { "半糖天气",     "陈一诺",   217 },
    { "城市灯火",     "白昼乐队", 229 },
    { "橘色海面",     "顾言",     184 },
    { "慢一点",       "木子",     205 },
    { "风从南边来",   "南山乐队", 238 },
    { "写在扉页",     "午后电台", 195 },
    // 经典老歌
    { "旧时月色",     "顾言",     246 },
    { "长街灯火",     "南山乐队", 231 },
    { "一封旧信",     "陈一诺",   219 },
    { "春天的窗口",   "林间",     208 },
    { "归来的船",     "小舟",     254 },
    { "那年冬天",     "木子",     227 },
    { "远方的钟",     "午后电台", 243 },
    { "河岸晚照",     "白昼乐队", 236 },
    // 轻音乐
    { "雨落窗台",     "午后电台", 262 },
    { "林间小径",     "林间",     248 },
    { "午后茶香",     "小舟",     231 },
    { "白色窗帘",     "顾言",     256 },
    { "山间薄雾",     "木子",     274 },
    { "静夜书页",     "陈一诺",   243 },
    { "水面微光",     "南山乐队", 267 },
    { "初雪",         "白昼乐队", 239 },
    // 电子节拍
    { "霓虹加速",     "白昼乐队", 201 },
    { "电流花园",     "南山乐队", 214 },
    { "午夜信号",     "木子",     188 },
    { "光轨",         "顾言",     196 },
    { "跳跃的方格",   "小舟",     207 },
    { "星屑回路",     "林间",     221 },
    { "低音街区",     "午后电台", 193 },
    { "重启",         "陈一诺",   182 },
    // 深夜电台
    { "零点以后",     "午后电台", 258 },
    { "空荡的站台",   "顾言",     247 },
    { "一盏路灯",     "木子",     269 },
    { "夜航",         "小舟",     252 },
    { "给明天的信",   "陈一诺",   261 },
    { "无声电影",     "林间",     244 },
    { "末班车上",     "南山乐队", 233 },
    { "天亮之前",     "白昼乐队", 276 },
};

void kg_catalog_default(kg_catalog_t *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->playlist_count = DEMO_PLAYLISTS;
    c->track_count = DEMO_TRACKS;

    for (uint8_t i = 0; i < DEMO_PLAYLISTS; i++) {
        kg_playlist_t *p = &c->playlists[i];
        snprintf(p->name, sizeof(p->name), "%s", k_lists[i].name);
        snprintf(p->subtitle, sizeof(p->subtitle), "%s", k_lists[i].subtitle);
        p->accent = k_lists[i].accent;
        p->first = (uint8_t)(i * DEMO_TRACKS_PER_LIST);
        p->count = DEMO_TRACKS_PER_LIST;
        p->total = DEMO_TRACKS_PER_LIST;
    }

    for (uint8_t i = 0; i < DEMO_TRACKS; i++) {
        kg_track_t *t = &c->tracks[i];
        snprintf(t->title, sizeof(t->title), "%s", k_tracks[i].title);
        snprintf(t->artist, sizeof(t->artist), "%s", k_tracks[i].artist);
        t->duration_s = k_tracks[i].duration_s;
        t->seed = i;
    }
}
