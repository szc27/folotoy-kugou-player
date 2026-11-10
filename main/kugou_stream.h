// main/kugou_stream.h —— HTTPS MP3 流播放（Helix 解码 + BSP ES8311 输出）。
//
// 下载与解码放在同一个播放任务里交替执行，输入缓冲有上限，绝不把整首歌
// 读进 RAM；PCM 写入是阻塞操作，只在播放任务里进行。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define KG_STREAM_URL_CAP 512

esp_err_t kg_stream_init(void);
void kg_stream_play(const char *url);
void kg_stream_pause(bool paused);
void kg_stream_stop(void);
void kg_stream_set_volume(uint8_t percent);

bool kg_stream_active(void);
bool kg_stream_eof(void);
uint32_t kg_stream_elapsed_ms(void);
