// main/kugou_wifi.c —— 见 kugou_wifi.h。
#include "kugou_wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "kg_wifi";

#define WIFI_GOT_IP   BIT0
#define WIFI_FAIL     BIT1
#define CONNECT_WAIT_MS 15000

static EventGroupHandle_t s_events;
static volatile kg_wifi_state_t s_state = KG_WIFI_IDLE;
static bool s_started;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (!s_events) return;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_state == KG_WIFI_CONNECTING) xEventGroupSetBits(s_events, WIFI_FAIL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_events, WIFI_GOT_IP);
    }
}

esp_err_t kg_wifi_init(void)
{
    if (s_started) return ESP_OK;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs init returned %d, continuing with existing data", (int)err);
    }
    (void)esp_netif_init();
    (void)esp_event_loop_create_default();
    if (!esp_netif_create_default_wifi_sta()) return ESP_FAIL;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;
    (void)esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              on_wifi_event, NULL, NULL);
    (void)esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              on_wifi_event, NULL, NULL);
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_start() != ESP_OK) {
        return ESP_FAIL;
    }
    s_started = true;
    return ESP_OK;
}

esp_err_t kg_wifi_scan(kg_ap_t *out, uint8_t *count, uint8_t cap)
{
    if (!out || !count) return ESP_ERR_INVALID_ARG;
    *count = 0;
    if (kg_wifi_init() != ESP_OK) return ESP_FAIL;
    s_state = KG_WIFI_SCANNING;

    wifi_scan_config_t scan = { .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) { s_state = KG_WIFI_FAILED; return err; }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) { s_state = KG_WIFI_IDLE; return ESP_OK; }
    uint16_t take = found > cap ? cap : found;
    wifi_ap_record_t *records = calloc(take, sizeof(wifi_ap_record_t));
    if (!records) { s_state = KG_WIFI_FAILED; return ESP_ERR_NO_MEM; }
    err = esp_wifi_scan_get_ap_records(&take, records);
    if (err == ESP_OK) {
        for (uint16_t i = 0; i < take; i++) {
            if (records[i].ssid[0] == '\0') continue;
            kg_ap_t *ap = &out[*count];
            snprintf(ap->ssid, sizeof(ap->ssid), "%s", (const char *)records[i].ssid);
            ap->rssi = records[i].rssi;
            ap->open = (records[i].authmode == WIFI_AUTH_OPEN);
            (*count)++;
        }
    }
    free(records);
    s_state = (err == ESP_OK) ? KG_WIFI_IDLE : KG_WIFI_FAILED;
    return err;
}

esp_err_t kg_wifi_connect(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || !pass) return ESP_ERR_INVALID_ARG;
    if (kg_wifi_init() != ESP_OK) return ESP_FAIL;
    s_state = KG_WIFI_CONNECTING;
    xEventGroupClearBits(s_events, WIFI_GOT_IP | WIFI_FAIL);

    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", ssid);
    snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%s", pass);
    // 接受任何加密方式（含开放网络）：threshold 设成 WPA2_PSK 会拒绝开放网络
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err != ESP_OK) { s_state = KG_WIFI_FAILED; return err; }

    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_GOT_IP | WIFI_FAIL,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(CONNECT_WAIT_MS));
    if (bits & WIFI_GOT_IP) {
        s_state = KG_WIFI_READY;
        ESP_LOGI(TAG, "wifi connected");
        return ESP_OK;
    }
    s_state = KG_WIFI_FAILED;
    ESP_LOGW(TAG, "wifi connect failed or timed out");
    return ESP_FAIL;
}

bool kg_wifi_ready(void) { return s_state == KG_WIFI_READY; }
kg_wifi_state_t kg_wifi_state(void) { return s_state; }

static nvs_handle_t open_nvs(nvs_open_mode_t mode)
{
    nvs_handle_t h = 0;
    if (nvs_open("kg_wifi", mode, &h) != ESP_OK) return 0;
    return h;
}

esp_err_t kg_wifi_load(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap)
{
    if (ssid && ssid_cap) ssid[0] = '\0';
    if (pass && pass_cap) pass[0] = '\0';
    nvs_handle_t h = open_nvs(NVS_READONLY);
    if (!h) return ESP_ERR_NOT_FOUND;
    if (ssid && ssid_cap) {
        size_t n = ssid_cap;
        if (nvs_get_str(h, "ssid", ssid, &n) != ESP_OK) ssid[0] = '\0';
    }
    if (pass && pass_cap) {
        size_t n = pass_cap;
        if (nvs_get_str(h, "pass", pass, &n) != ESP_OK) pass[0] = '\0';
    }
    nvs_close(h);
    return ESP_OK;
}

esp_err_t kg_wifi_save(const char *ssid, const char *pass)
{
    nvs_handle_t h = open_nvs(NVS_READWRITE);
    if (!h) return ESP_FAIL;
    esp_err_t a = nvs_set_str(h, "ssid", ssid ? ssid : "");
    esp_err_t b = nvs_set_str(h, "pass", pass ? pass : "");
    esp_err_t c = nvs_commit(h);
    nvs_close(h);
    return (a == ESP_OK && b == ESP_OK && c == ESP_OK) ? ESP_OK : ESP_FAIL;
}

void kg_wifi_clear(void)
{
    nvs_handle_t h = open_nvs(NVS_READWRITE);
    if (!h) return;
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "pass");
    nvs_commit(h);
    nvs_close(h);
}
