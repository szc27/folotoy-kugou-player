// main/kugou_stream.c —— 见 kugou_stream.h。
#include "kugou_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_audio.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mp3dec.h"

static const char *TAG = "kg_stream";

#define INPUT_BYTES      (4 * 1024)
#define PCM_SAMPLES      2304
#define READ_CHUNK       1024
#define STACK_BYTES      12288
#define PREBUFFER_BYTES  (3 * 1024)
#define DEFAULT_VOLUME   70

static uint8_t s_input[INPUT_BYTES];
static int16_t s_pcm[PCM_SAMPLES];
static char s_url[KG_STREAM_URL_CAP];
static volatile bool s_active;
static volatile bool s_paused;
static volatile bool s_cancel;
static volatile bool s_eof;
static volatile uint8_t s_volume = DEFAULT_VOLUME;
static volatile uint32_t s_elapsed_ms;
static volatile uint32_t s_generation;
static TaskHandle_t s_task;
static bool s_audio_ready;

static int fill_input(esp_http_client_handle_t client, int bytes_left,
                      uint8_t **read_ptr)
{
    if (bytes_left > 0 && *read_ptr != s_input) {
        memmove(s_input, *read_ptr, (size_t)bytes_left);
    }
    *read_ptr = s_input;
    int space = INPUT_BYTES - bytes_left;
    if (space <= 0) return bytes_left;
    // 音频流经弱网/模拟器网络桥时可能偶发 EAGAIN(读超时)，
    // 重试几次而非立即判结束，能显著提高弱网下的续播稳定性。
    for (int retry = 0; retry < 6; retry++) {
        int got = esp_http_client_read(client, (char *)(s_input + bytes_left),
                                       space > READ_CHUNK ? READ_CHUNK : space);
        if (got > 0) return bytes_left + got;
        if (got != -ESP_ERR_HTTP_EAGAIN && got != -ESP_ERR_HTTP_CONNECTION_CLOSED) {
            ESP_LOGW(TAG, "fill_input got=%d space=%d", got, space);
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGW(TAG, "fill_input EAGAIN retries exhausted");
    return -1;  // 结束或出错
}

static void write_pcm(const int16_t *samples, int count, int channels, int rate)
{
    if (count <= 0 || rate <= 0) return;
    int16_t mono[PCM_SAMPLES];
    int out = 0;
    if (channels == 2) {
        for (int i = 0; i + 1 < count; i += 2) {
            int32_t v = ((int32_t)samples[i] + (int32_t)samples[i + 1]) / 2;
            mono[out++] = (int16_t)(v * (int32_t)s_volume / 100);
        }
    } else {
        for (int i = 0; i < count; i++) {
            mono[out++] = (int16_t)((int32_t)samples[i] * (int32_t)s_volume / 100);
        }
    }
    if (out == 0) return;
    if (bsp_audio_write(mono, (size_t)out * sizeof(int16_t)) == ESP_OK) {
        s_elapsed_ms += (uint32_t)((uint64_t)out * 1000u / (uint32_t)rate);
    }
}

static void play_url(const char *url, uint32_t generation)
{
    HMP3Decoder decoder = MP3InitDecoder();
    if (!decoder) { ESP_LOGE(TAG, "mp3 decoder alloc failed"); return; }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 20000,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .max_redirection_count = 5,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { MP3FreeDecoder(decoder); return; }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        int status = esp_http_client_fetch_headers(client);
        err = esp_http_client_get_status_code(client);
        if (err < 200 || err >= 300) {
            ESP_LOGW(TAG, "audio http status=%d headers=%d", err, status);
            err = ESP_FAIL;
        } else {
            err = ESP_OK;
        }
    }
    if (err != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        MP3FreeDecoder(decoder);
        ESP_LOGE(TAG, "audio stream open failed");
        s_eof = true;
        return;
    }

    uint8_t *read_ptr = s_input;
    int bytes_left = 0;
    bool eof = false;
    bool prebuffer = true;
    int format_rate = 0;
    int decode_errors = 0;

    while (!s_cancel && !eof && generation == s_generation) {
        if (s_paused) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }

        if (bytes_left < (prebuffer ? PREBUFFER_BYTES : INPUT_BYTES / 2)) {
            int next = fill_input(client, bytes_left, &read_ptr);
            if (next < 0) { eof = true; break; }
            bytes_left = next;
            if (prebuffer && bytes_left >= PREBUFFER_BYTES) prebuffer = false;
        }

        int sync = MP3FindSyncWord(read_ptr, bytes_left);
        if (sync < 0) {
            int next = fill_input(client, bytes_left, &read_ptr);
            if (next < 0) { eof = true; break; }
            bytes_left = next;
            continue;
        }
        read_ptr += sync;
        bytes_left -= sync;

        int err_decode = MP3Decode(decoder, &read_ptr, &bytes_left, s_pcm, 0);
        if (err_decode != ERR_MP3_NONE) {
            decode_errors++;
            if (decode_errors > 64) { ESP_LOGE(TAG, "too many decode errors"); break; }
            if (bytes_left <= 0) {
                int next = fill_input(client, bytes_left, &read_ptr);
                if (next < 0) { eof = true; break; }
                bytes_left = next;
            }
            continue;
        }
        decode_errors = 0;

        MP3FrameInfo info;
        MP3GetLastFrameInfo(decoder, &info);
        if (info.samprate == 0 || info.outputSamps <= 0) {
            continue;
        }
        int rate = info.samprate;
        if (rate != format_rate) {
            // 格式切换前必须停止 PCM 读写，BSP 内部会 close 再 open
            format_rate = rate;
            if (bsp_audio_set_format((uint32_t)rate, 16, 1) != ESP_OK) {
                ESP_LOGE(TAG, "unsupported sample rate %d", rate);
                break;
            }
        }
        int count = info.outputSamps;
        if (count > PCM_SAMPLES) count = PCM_SAMPLES;
        write_pcm(s_pcm, count, info.nChans, rate);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    MP3FreeDecoder(decoder);
    s_eof = true;
    ESP_LOGI(TAG, "stream ended, elapsed=%u ms", (unsigned)s_elapsed_ms);
}

static void stream_task(void *arg)
{
    (void)arg;
    char url[KG_STREAM_URL_CAP];
    while (1) {
        if (!s_active) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        snprintf(url, sizeof(url), "%s", s_url);
        uint32_t generation = s_generation;
        s_cancel = false;
        s_eof = false;
        s_elapsed_ms = 0;
        play_url(url, generation);
        // 只有"同一代播放自然结束"才清除 active；切歌时 generation 已变，
        // 让循环立即用新 URL 重新开始，避免短暂把状态置回停止。
        if (generation == s_generation && !s_cancel) s_active = false;
    }
}

static void ensure_audio(void)
{
    if (s_audio_ready) return;
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "codec init failed");
        return;
    }
    bsp_audio_set_volume(100);  // 数字音量在 PCM 上做，codec 保持满幅
    s_audio_ready = true;
    ESP_LOGI(TAG, "audio codec ready on first play");
}

esp_err_t kg_stream_init(void)
{
    // 音频 codec 也懒初始化：首次播放才分配 I2S DMA buffer(~11.5KB)，
    // 启动时空出 heap 给 Wi-Fi 的 static RX buffer(其硬性需 6×1600)。
    // stream_task 同样在首次 kg_stream_play 时懒创建(12KB 栈)。
    ESP_LOGI(TAG, "mp3 stream deferred (helix %d B input, %d samples PCM)",
             INPUT_BYTES, PCM_SAMPLES);
    return ESP_OK;
}

void kg_stream_play(const char *url)
{
    if (!url || !url[0]) return;
    ESP_LOGI(TAG, "play url: %.180s", url);
    ensure_audio();            // 首次播放才 init 音频 codec(省启动 heap)
    s_generation++;            // 通知正在播放的任务退出旧流
    snprintf(s_url, sizeof(s_url), "%s", url);
    s_paused = false;
    s_eof = false;
    s_active = true;
    if (!s_task) {
        // 懒创建：首次播放才占 12KB 栈
        if (xTaskCreate(stream_task, "kg_stream", STACK_BYTES, NULL, 6, &s_task) != pdPASS) {
            ESP_LOGE(TAG, "stream task create failed (no mem)");
        }
    }
}

void kg_stream_pause(bool paused) { s_paused = paused; }
void kg_stream_stop(void)
{
    s_generation++;
    s_cancel = true;
    s_active = false;
    s_paused = false;
}
void kg_stream_set_volume(uint8_t percent)
{
    s_volume = percent > 100 ? 100 : percent;
}
bool kg_stream_active(void) { return s_active; }
bool kg_stream_eof(void) { return s_eof && !s_active; }
uint32_t kg_stream_elapsed_ms(void) { return s_elapsed_ms; }
