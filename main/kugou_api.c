// main/kugou_api.c —— 设备端酷狗直连客户端。
//
// 协议行为按 MIT 许可的 KuGouMusicApi（280867678/KuGouMusicApi）2026-11 版本
// 独立实现：公共参数 + MD5 签名，直接请求 login-user.kugou.com /
// gateway.kugou.com，不经任何中转服务。仓库内不含任何账号凭据。
//
// 所有网络与加解密都在单 worker 任务里执行；大块缓冲放 BSS，避免 C3 栈溢出。
#include "kugou_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_font_coverage.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "kugou_sign.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "kg_api";

#define HOST_GATEWAY      "https://gateway.kugou.com"
#define HOST_LOGIN        "https://login-user.kugou.com"
#define HOST_USER_SERVICE "https://userservice.kugou.com"
#define RESP_CAP          16384
#define POLL_MS           2000
#define URL_CAP           1024

// /v5/url 的 signKey 盐值随平台而异：标准版(非 lite)用 57ae12...，
// lite 版才用 185672dd...。本固件 appid=1005 属标准版，用前者。
#define KG_SALT_V5URL_STD "57ae12eb6890223e355ccfcb74edf70d"
#define KG_V5URL_VERSION  "11040"

// 联调免扫码登录开关。⚠️ 仅联调使用：
//   - 定义 KG_DEV_DEBUG_LOGIN 时，刷固件清空 NVS 后会用下面的 userid/token 自动
//     注入登录态，省去反复扫码；
//   - 不定义该宏（默认）时，两个常量均为空、不注入任何账号信息。
// 真实刷机 / 正式发布构建必须保持「未定义」，确保固件不含真实 userid/token。
#ifdef KG_DEV_DEBUG_LOGIN
#define KG_DEV_USERID   "413887693"
#define KG_DEV_TOKEN    "141ff424a1474b3069b1690a7063692c2c09ee868b5a3c64f8e8bd9da7485dd8"
#else
#define KG_DEV_USERID   ""
#define KG_DEV_TOKEN    ""
#endif

typedef struct {
    char token[256];
    char userid[24];
    char nickname[KG_NAME_CAP];
} kg_session_t;

typedef struct { char *data; size_t cap; size_t len; } http_buf_t;

static kg_event_cb_t s_cb;
static void *s_user;
static QueueHandle_t s_cmd_q;
static kg_session_t s_session;
static kg_common_t s_common;
static char s_dfid[64];  // 设备指纹：register_dev 返回后持久化，供 mid=md5(dfid) 签名用
static char s_resp[RESP_CAP];
static kg_event_t s_event;
static char s_qr_key[KG_QR_KEY_CAP];
static bool s_polling;
static bool s_has_session;
static kg_catalog_t s_catalog;

bool kg_api_has_session(void) { return s_has_session; }

const kg_catalog_t *kg_api_catalog(void) { return &s_catalog; }

static void post(void) { if (s_cb) s_cb(&s_event, s_user); }

static void post_wifi(kg_wifi_state_t state, const kg_ap_t *aps, uint8_t count,
                      const char *message)
{
    memset(&s_event, 0, sizeof(s_event));
    s_event.type = KG_EV_WIFI;
    s_event.wifi = state;
    if (aps && count) {
        if (count > KG_WIFI_AP_MAX) count = KG_WIFI_AP_MAX;
        memcpy(s_event.aps, aps, count * sizeof(kg_ap_t));
        s_event.ap_count = count;
    }
    snprintf(s_event.message, sizeof(s_event.message), "%s", message ? message : "");
    post();
}

static void post_message(const char *message)
{
    memset(&s_event, 0, sizeof(s_event));
    s_event.type = KG_EV_MESSAGE;
    snprintf(s_event.message, sizeof(s_event.message), "%s", message ? message : "");
    post();
}

static void post_qr(const char *url)
{
    memset(&s_event, 0, sizeof(s_event));
    s_event.type = KG_EV_QR;
    snprintf(s_event.qr_url, sizeof(s_event.qr_url), "%s", url ? url : "");
    post();
}

static void post_login(kg_login_t login)
{
    memset(&s_event, 0, sizeof(s_event));
    s_event.type = KG_EV_LOGIN;
    s_event.login = login;
    snprintf(s_event.nickname, sizeof(s_event.nickname), "%s", s_session.nickname);
    post();
}

// ---------------------------------------------------------------------------
// 字库过滤：在线返回的生僻字不能上屏变豆腐块
// ---------------------------------------------------------------------------

static size_t utf8_decode(const char *s, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    if ((p[0] & 0xE0) == 0xC0 && p[1]) {
        *cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F); return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
        *cp = ((uint32_t)(p[0] & 0x0F) << 12) |
              ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); return 3;
    }
    *cp = '?'; return 1;
}

static void copy_safe(char *dst, size_t cap, const char *src)
{
    if (cap == 0) return;
    size_t used = 0;
    if (!src) src = "";
    while (*src && used + 5 < cap) {
        uint32_t cp = 0;
        size_t n = utf8_decode(src, &cp);
        bool ok = app_font_covers(cp);
        size_t put = ok ? n : 1;
        if (used + put + 1 > cap) break;
        memcpy(dst + used, ok ? src : "?", put);
        used += put;
        src += n;
    }
    dst[used] = '\0';
}

// ---------------------------------------------------------------------------
// 会话 NVS
// ---------------------------------------------------------------------------

static void session_load(void)
{
    memset(&s_session, 0, sizeof(s_session));
    nvs_handle_t h = 0;
    if (nvs_open("kg_session", NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(s_session.token);
        if (nvs_get_str(h, "token", s_session.token, &n) != ESP_OK) s_session.token[0] = '\0';
        n = sizeof(s_session.userid);
        if (nvs_get_str(h, "userid", s_session.userid, &n) != ESP_OK) s_session.userid[0] = '\0';
        n = sizeof(s_session.nickname);
        if (nvs_get_str(h, "name", s_session.nickname, &n) != ESP_OK) s_session.nickname[0] = '\0';
        nvs_close(h);
    }
    // 开发调试：NVS 被清空时用编译期常量注入，避免反复扫码。
    if (s_session.token[0] == '\0' && KG_DEV_TOKEN[0]) {
        snprintf(s_session.token, sizeof(s_session.token), "%s", KG_DEV_TOKEN);
        snprintf(s_session.userid, sizeof(s_session.userid), "%s", KG_DEV_USERID);
    }
    // token 与 userid 必须同时有效才算登录态：历史版本 userid 提取失败时
    // 会存下"空 userid + 有 token"的残缺会话，签名会因此报 20010。
    s_has_session = s_session.token[0] != '\0' && s_session.userid[0] != '\0';
    ESP_LOGI(TAG, "session %s", s_has_session ? "restored" : "absent");
}

static void session_save(void)
{
    nvs_handle_t h = 0;
    if (nvs_open("kg_session", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "token", s_session.token);
    nvs_set_str(h, "userid", s_session.userid);
    nvs_set_str(h, "name", s_session.nickname);
    nvs_commit(h);
    nvs_close(h);
    s_has_session = s_session.token[0] != '\0';
}

static void dfid_load(void)
{
    nvs_handle_t h = 0;
    if (nvs_open("kg_dev", NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(s_dfid);
        if (nvs_get_str(h, "dfid", s_dfid, &n) != ESP_OK) s_dfid[0] = '\0';
        nvs_close(h);
    }
}

static void dfid_save(void)
{
    nvs_handle_t h = 0;
    if (nvs_open("kg_dev", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "dfid", s_dfid);
        nvs_commit(h);
        nvs_close(h);
    }
}

// 标准 base64 编码（register_dev 的 body 用，避免引入 mbedtls 组件依赖）。
static size_t b64_encode(const unsigned char *src, size_t slen, char *dst, size_t dcap)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t olen = 0;
    for (size_t i = 0; i < slen; i += 3) {
        unsigned a = src[i];
        unsigned b = (i + 1 < slen) ? src[i + 1] : 0;
        unsigned c = (i + 2 < slen) ? src[i + 2] : 0;
        if (olen + 4 >= dcap) return 0;
        dst[olen++] = tbl[a >> 2];
        dst[olen++] = tbl[((a & 3) << 4) | (b >> 4)];
        dst[olen++] = (i + 1 < slen) ? tbl[((b & 15) << 2) | (c >> 6)] : '=';
        dst[olen++] = (i + 2 < slen) ? tbl[c & 63] : '=';
    }
    dst[olen] = '\0';
    return olen;
}

static void session_clear(void)
{
    memset(&s_session, 0, sizeof(s_session));
    nvs_handle_t h = 0;
    if (nvs_open("kg_session", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    s_has_session = false;
    // 清空在线曲库，避免换账号后残留上一账号的歌单/曲目
    s_catalog.playlist_count = 0;
    s_catalog.track_count = 0;
}

// ---------------------------------------------------------------------------
// HTTP：公共参数 + 签名 + 固定客户端头
// ---------------------------------------------------------------------------

static esp_err_t on_http_data(esp_http_client_event_t *evt)
{
    http_buf_t *b = (http_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && b && evt->data_len > 0) {
        size_t room = (b->cap > b->len + 1) ? b->cap - b->len - 1 : 0;
        size_t take = evt->data_len < room ? evt->data_len : room;
        if (take) {
            memcpy(b->data + b->len, evt->data, take);
            b->len += take;
            b->data[b->len] = '\0';
        }
    }
    return ESP_OK;
}

static void add_param(kg_param_t *p, size_t *n, size_t cap,
                      const char *key, const char *value)
{
    for (size_t i = 0; i < *n; i++) {
        if (strcmp(p[i].key, key) == 0) { p[i].value = value; return; }
    }
    if (*n < cap) { p[*n].key = key; p[*n].value = value; (*n)++; }
}

// android=1 用 Android 盐并把 POST body 纳入签名；0 用 Web 盐。
static int kg_request(const char *base, const char *path, const char *method,
                      kg_param_t *biz, size_t biz_n, int android,
                      const char *body, const char *xrouter)
{
    s_resp[0] = '\0';
    kg_common_init(&s_common, s_dfid[0] ? s_dfid : "-",
                   s_session.userid, s_session.token, (uint32_t)time(NULL));
    kg_param_t params[KG_PARAM_MAX];
    size_t n = kg_common_params(&s_common, params, KG_PARAM_MAX);
    for (size_t i = 0; i < biz_n; i++) {
        add_param(params, &n, KG_PARAM_MAX, biz[i].key, biz[i].value);
    }

    char sig[KG_MD5_HEX_CAP];
    const char *sig_ptr = NULL;
    if (android > 0) { kg_sign_android(params, n, body, sig); sig_ptr = sig; }
    else if (android == 0) { kg_sign_web(params, n, sig); sig_ptr = sig; }
    // android < 0：notSign —— 部分接口只带 encryptKey，不生成 signature

    char url[URL_CAP];
    if (kg_build_url(url, sizeof(url), base, path, params, n, sig_ptr) < 0) return -1;

    http_buf_t buf = { .data = s_resp, .cap = RESP_CAP, .len = 0 };
    esp_http_client_config_t cfg = {
        .url = url,
        .method = (method && strcmp(method, "POST") == 0) ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .event_handler = on_http_data,
        .user_data = &buf,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;
    esp_http_client_set_header(client, "User-Agent",
                               "Android15-1070-11083-46-0-DiscoveryDRADProtocol-wifi");
    esp_http_client_set_header(client, "dfid", s_common.dfid);
    esp_http_client_set_header(client, "mid", s_common.mid);
    esp_http_client_set_header(client, "clienttime", s_common.clienttime);
    if (xrouter) esp_http_client_set_header(client, "x-router", xrouter);
    if (s_session.token[0]) {
        char cookie[512];
        snprintf(cookie, sizeof(cookie), "userid=%s; token=%s",
                 s_session.userid, s_session.token);
        esp_http_client_set_header(client, "Cookie", cookie);
    }
    if (body && body[0]) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, (int)strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(TAG, "request failed: err=%d status=%d path=%s", (int)err, status, path);
        return -1;
    }
    return (int)buf.len;
}

// ---------------------------------------------------------------------------
// JSON 辅助：上游字段嵌套可能变化，用宽容解析提高可用性
// ---------------------------------------------------------------------------

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

static cJSON *find_array(cJSON *node, const char *item_key)
{
    if (!node) return NULL;
    if (cJSON_IsArray(node) && cJSON_GetArraySize(node) > 0) {
        cJSON *first = cJSON_GetArrayItem(node, 0);
        if (cJSON_IsObject(first) && cJSON_GetObjectItemCaseSensitive(first, item_key)) {
            return node;
        }
    }
    cJSON *child = NULL;
    cJSON_ArrayForEach(child, node) {
        cJSON *hit = find_array(child, item_key);
        if (hit) return hit;
    }
    return NULL;
}

static const char *find_string_field(cJSON *node, const char *key)
{
    if (!node) return NULL;
    if (cJSON_IsObject(node)) {
        const cJSON *it = cJSON_GetObjectItemCaseSensitive(node, key);
        if (cJSON_IsString(it) && it->valuestring && it->valuestring[0]) return it->valuestring;
    }
    cJSON *child = NULL;
    cJSON_ArrayForEach(child, node) {
        const char *hit = find_string_field(child, key);
        if (hit) return hit;
    }
    return NULL;
}

// 递归查找字符串或数字字段（userid 可能是数字，find_string_field 只认字符串）。
static const cJSON *find_scalar_field(cJSON *node, const char *key)
{
    if (!node) return NULL;
    if (cJSON_IsObject(node)) {
        const cJSON *it = cJSON_GetObjectItemCaseSensitive(node, key);
        if (it && (cJSON_IsString(it) || cJSON_IsNumber(it))) return it;
    }
    cJSON *child = NULL;
    cJSON_ArrayForEach(child, node) {
        const cJSON *hit = find_scalar_field(child, key);
        if (hit) return hit;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// 设备注册：向风险服务登记设备指纹，拿到真实 dfid。
// /v5/url 要求真实 dfid（mid=md5(dfid)），用 "-" 会返回 err signature。
// ---------------------------------------------------------------------------
static int register_dev(void)
{
    const char *dfid = s_dfid[0] ? s_dfid : "-";
    char clienttime[24];
    snprintf(clienttime, sizeof(clienttime), "%lu", (unsigned long)time(NULL));

    // body = base64(JSON)，首次注册 mid/uuid 留空
    char json[256];
    snprintf(json, sizeof(json),
             "{\"mid\":\"\",\"uuid\":\"\",\"appid\":\"1014\",\"userid\":\"0\"}");
    char b64[384];
    if (b64_encode((const unsigned char *)json, strlen(json), b64, sizeof(b64)) == 0) {
        return -1;
    }

    kg_param_t params[] = {
        { "dfid", dfid }, { "mid", "" }, { "uuid", "" },
        { "appid", "1014" }, { "clientver", KG_CLIENTVER },
        { "userid", "0" }, { "clienttime", clienttime },
        { "p.token", "" }, { "platid", "4" },
    };
    char sig[KG_MD5_HEX_CAP];
    kg_sign_register(params, 9, sig);

    char url[URL_CAP];
    if (kg_build_url(url, sizeof(url), HOST_USER_SERVICE,
                     "/risk/v1/r_register_dev", params, 9, sig) < 0) return -1;

    s_resp[0] = '\0';
    http_buf_t buf = { .data = s_resp, .cap = RESP_CAP, .len = 0 };
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = on_http_data,
        .user_data = &buf,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;
    esp_http_client_set_header(client, "User-Agent",
                               "Android15-1070-11083-46-0-DiscoveryDRADProtocol-wifi");
    esp_http_client_set_header(client, "dfid", dfid);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, b64, (int)strlen(b64));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "register_dev status=%d err=%d resp=%.200s", status, (int)err, s_resp);
    if (err != ESP_OK || status < 200 || status >= 300) return -1;

    cJSON *root = cJSON_Parse(s_resp);
    const char *nd = root ? find_string_field(root, "dfid") : NULL;
    if (!nd) nd = root ? find_string_field(root, "dFid") : NULL;
    if (nd && nd[0]) {
        snprintf(s_dfid, sizeof(s_dfid), "%s", nd);
        dfid_save();
        ESP_LOGI(TAG, "register_dev got dfid=%s", s_dfid);
    }
    cJSON_Delete(root);
    return nd ? 0 : -1;
}

// ---------------------------------------------------------------------------
// 业务接口
// ---------------------------------------------------------------------------

static bool fetch_qr_key(char *url_out, size_t cap)
{
    char qrcode_txt[192];
    snprintf(qrcode_txt, sizeof(qrcode_txt),
             "https://h5.kugou.com/apps/loginQRCode/html/index.html?appid=%s&",
             KG_APPID);
    kg_param_t biz[] = {
        { "appid", KG_LOGIN_APPID },
        { "type", "1" },
        { "plat", "4" },
        { "srcappid", KG_SRCAPPID },
        { "qrcode_txt", qrcode_txt },
    };
    if (kg_request(HOST_LOGIN, "/v2/qrcode", "GET", biz, 5, 0, NULL, NULL) < 0) {
        return false;
    }
    char key[KG_QR_KEY_CAP];
    const char *fields[] = { "qrcode", "unikey", "key", "qrcode_txt" };
    bool found = false;
    for (size_t i = 0; i < 4 && !found; i++) {
        found = (kg_proto_extract_string(s_resp, fields[i], key, sizeof(key)) == 0 &&
                 key[0] != '\0');
    }
    if (!found) {
        cJSON *root = cJSON_Parse(s_resp);
        const char *hit = find_string_field(root, "qrcode");
        if (hit) { snprintf(key, sizeof(key), "%s", hit); found = true; }
        cJSON_Delete(root);
    }
    if (!found) return false;
    snprintf(s_qr_key, sizeof(s_qr_key), "%s", key);
    return kg_proto_build_login_url(url_out, cap,
                                    CONFIG_KUGOU_QR_URL_TEMPLATE, key) > 0;
}

static kg_login_t poll_qr(void)
{
    kg_param_t biz[] = {
        { "plat", "4" },
        { "appid", KG_APPID },
        { "srcappid", KG_SRCAPPID },
        { "qrcode", s_qr_key },
    };
    if (kg_request(HOST_LOGIN, "/v2/get_userinfo_qrcode", "GET",
                   biz, 4, 0, NULL, NULL) < 0) return KG_LOGIN_ERROR;
    int status = -1;
    if (kg_proto_extract_int(s_resp, "status", &status) != 0) {
        cJSON *root = cJSON_Parse(s_resp);
        const cJSON *st = root ? cJSON_GetObjectItemCaseSensitive(root, "status") : NULL;
        if (cJSON_IsNumber(st)) status = st->valueint;
        cJSON_Delete(root);
    }
    kg_login_t result = kg_proto_login_state(status);
    if (result == KG_LOGIN_DONE) {
        char token[256] = "", userid[24] = "", name[KG_NAME_CAP] = "";
        kg_proto_extract_string(s_resp, "token", token, sizeof(token));
        cJSON *root = cJSON_Parse(s_resp);
        const cJSON *uid_item = find_scalar_field(root, "userid");
        const char *nick = find_string_field(root, "nickname");
        if (!nick) nick = find_string_field(root, "username");
        if (cJSON_IsString(uid_item)) {
            snprintf(userid, sizeof(userid), "%s", uid_item->valuestring);
        } else if (cJSON_IsNumber(uid_item)) {
            snprintf(userid, sizeof(userid), "%lld", (long long)uid_item->valuedouble);
        }
        if (nick) copy_safe(name, sizeof(name), nick);
        cJSON_Delete(root);
        if (token[0]) {
            snprintf(s_session.token, sizeof(s_session.token), "%s", token);
            if (userid[0]) snprintf(s_session.userid, sizeof(s_session.userid), "%s", userid);
            snprintf(s_session.nickname, sizeof(s_session.nickname), "%s", name);
            session_save();
            // 打印登录态，便于开发期抓取后回填 KG_DEV_USERID/KG_DEV_TOKEN 免扫码。
            // 仅在联调宏开启时打印，正式构建不输出 token 明文。
#ifdef KG_DEV_DEBUG_LOGIN
            ESP_LOGI(TAG, "LOGIN_OK userid=%s token=%s", s_session.userid, s_session.token);
#endif
        } else {
            return KG_LOGIN_ERROR;  // 有状态但没拿到 token，不伪造登录
        }
    }
    return result;
}

static void fetch_playlists(void)
{
    int page = (s_catalog.playlist_count / KG_PAGE_SIZE) + 1;
    char body[512];
    snprintf(body, sizeof(body),
             "{\"userid\":\"%s\",\"token\":\"%s\",\"total_ver\":0,"
             "\"type\":2,\"page\":%d,\"pagesize\":%d}",
             s_session.userid, s_session.token, page, KG_PAGE_SIZE);
    kg_param_t biz[] = {
        { "plat", "1" },
        { "userid", s_session.userid },
        { "token", s_session.token },
    };
    // /cloudlist.service/v8/get_all_list（路径含 service 名，无需 x-router），
    // total_ver=0 表示无本地基线、拉全量歌单。分批：每批 KG_PAGE_SIZE 个。
    if (kg_request(HOST_GATEWAY, "/cloudlist.service/v8/get_all_list", "POST",
                   biz, 3, 1, body, NULL) < 0) {
        post_message("歌单获取失败 请检查网络");
        return;
    }
    cJSON *root = cJSON_Parse(s_resp);
    cJSON *arr = find_array(root, "global_collection_id");
    if (!arr) arr = find_array(root, "name");
    if (!arr) { cJSON_Delete(root); post_message("歌单为空 或接口已变化"); return; }

    int count = cJSON_GetArraySize(arr);
    int added = 0;
    for (int i = 0; i < count && s_catalog.playlist_count < KG_PLAYLIST_MAX; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        const char *name = json_str(it, "name");
        if (!name) name = json_str(it, "list_name");
        if (!name || !name[0]) continue;
        uint8_t idx = s_catalog.playlist_count;
        kg_playlist_t *pl = &s_catalog.playlists[idx];
        copy_safe(pl->name, sizeof(pl->name), name);
        snprintf(pl->subtitle, sizeof(pl->subtitle), "在线歌单");
        pl->accent = 0x2CA8FF;
        pl->first = (uint8_t)(idx * KG_TRACKS_PER_LIST);
        pl->count = 0;
        pl->total = 0;
        const cJSON *gid = cJSON_GetObjectItemCaseSensitive(it, "global_collection_id");
        if (cJSON_IsString(gid)) {
            snprintf(pl->collection_id, sizeof(pl->collection_id), "%s", gid->valuestring);
        } else if (cJSON_IsNumber(gid)) {
            snprintf(pl->collection_id, sizeof(pl->collection_id), "%d", gid->valueint);
        }
        const cJSON *mc = cJSON_GetObjectItemCaseSensitive(it, "m_count");
        if (cJSON_IsNumber(mc) && mc->valueint > 0) pl->total = (uint16_t)mc->valueint;
        s_catalog.playlist_count++;
        added++;
    }
    cJSON_Delete(root);
    if (added == 0) {
        // 接口返回空：没有更多歌单，标记翻页终止
        memset(&s_event, 0, sizeof(s_event));
        s_event.type = KG_EV_CATALOG;
        s_event.playlist_index = -1;
        s_event.exhausted = true;
        post();
        return;
    }

    memset(&s_event, 0, sizeof(s_event));
    s_event.type = KG_EV_CATALOG;
    s_event.playlist_index = -1;  // 追加批次，不强制跳转光标
    post();
}

static void fetch_tracks(const kg_cmd_t *cmd)
{
    char id[64];
    snprintf(id, sizeof(id), "%s", cmd->extra);
    int pl = cmd->playlist_index;
    if (pl < 0 || pl >= (int)s_catalog.playlist_count) pl = 0;
    kg_playlist_t *list = &s_catalog.playlists[pl];
    int base = pl * KG_TRACKS_PER_LIST;
    // 分页：从已加载的曲目数推断下一页（每批 KG_PAGE_SIZE 首）
    int page = (list->count / KG_PAGE_SIZE) + 1;
    char pagesize[8], page_str[8];
    snprintf(pagesize, sizeof(pagesize), "%d", KG_PAGE_SIZE);
    snprintf(page_str, sizeof(page_str), "%d", page);
    // 2026-11 最新：/pubsongs/v4/get_other_list_file（旧 v2 nofilt 已废弃）
    kg_param_t biz[] = {
        { "specialid", "" }, { "need_sort", "1" },
        { "module", "CloudMusic" },
        { "pagesize", pagesize }, { "global_collection_id", id },
        { "userid", "0" }, { "mode", "1" },
        { "page", page_str }, { "type", "0" },
        { "area_code", "1" },
    };
    if (kg_request(HOST_GATEWAY, "/pubsongs/v4/get_other_list_file",
                   "GET", biz, 10, 1, NULL, NULL) < 0) {
        post_message("曲目获取失败");
        return;
    }
    cJSON *root = cJSON_Parse(s_resp);
    if (!root) {
        ESP_LOGE(TAG, "cJSON fail near: %.120s", cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "?");
    }
    cJSON *arr = find_array(root, "hash");
    if (!arr) arr = find_array(root, "album_audio_id");
    if (!arr) { cJSON_Delete(root); post_message("曲目为空 或接口已变化"); return; }

    // 存歌单总曲目数（响应 data.count）
    cJSON *data = root ? cJSON_GetObjectItemCaseSensitive(root, "data") : NULL;
    const cJSON *total = data ? cJSON_GetObjectItemCaseSensitive(data, "count") : NULL;
    if (cJSON_IsNumber(total) && total->valueint > 0) list->total = (uint16_t)total->valueint;

    // 追加到已有曲目之后，不重置 count
    int count = cJSON_GetArraySize(arr);
    int added = 0;
    for (int i = 0; i < count && list->count < KG_TRACKS_PER_LIST; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        const char *name = json_str(it, "name");
        if (!name) name = json_str(it, "filename");
        if (!name || !name[0]) continue;
        int idx = base + list->count;
        if (idx >= KG_TRACK_MAX) break;
        kg_track_t *t = &s_catalog.tracks[idx];
        copy_safe(t->title, sizeof(t->title), name);
        const char *artist = json_str(it, "singername");
        if (!artist) artist = json_str(it, "author");
        copy_safe(t->artist, sizeof(t->artist), artist ? artist : "未知歌手");
        const cJSON *dur = cJSON_GetObjectItemCaseSensitive(it, "duration");
        t->duration_s = (cJSON_IsNumber(dur) && dur->valueint > 0)
                            ? (uint16_t)dur->valueint : 200;
        t->seed = (uint8_t)idx;
        const char *h = json_str(it, "hash");
        if (h) snprintf(t->hash, sizeof(t->hash), "%s", h);
        const cJSON *aaid = cJSON_GetObjectItemCaseSensitive(it, "album_audio_id");
        if (cJSON_IsNumber(aaid)) {
            snprintf(t->album_audio_id, sizeof(t->album_audio_id), "%d", aaid->valueint);
        } else if (cJSON_IsString(aaid)) {
            snprintf(t->album_audio_id, sizeof(t->album_audio_id), "%s", aaid->valuestring);
        }
        ESP_LOGI(TAG, "track[%d] name=%.40s hash=%.40s aaid=%s", idx, name, t->hash, t->album_audio_id);
        list->count++;
        added++;
    }
    if (added > 0) {
        int end = base + list->count;
        if (end > s_catalog.track_count) s_catalog.track_count = (uint8_t)end;
    }
    cJSON_Delete(root);
    if (added == 0) {
        // 没有更多曲目：把 total 收敛到 count，避免反复翻页请求
        if (list->total > list->count) list->total = list->count;
        post_message("没有更多曲目");
        return;
    }

    memset(&s_event, 0, sizeof(s_event));
    s_event.type = KG_EV_CATALOG;
    s_event.playlist_index = pl;
    post();
}

static void fetch_track_url(const kg_cmd_t *cmd)
{
    // /v5/url 需要曲目 hash；hash 由 UI 侧从曲目列表缓存传入。
    char hash[64], album_id[64];
    snprintf(hash, sizeof(hash), "%s", cmd->key);
    for (char *p = hash; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;  // 对齐参考实现的 toLowerCase
    snprintf(album_id, sizeof(album_id), "%s", cmd->extra[0] ? cmd->extra : "0");
    char signkey[KG_MD5_HEX_CAP];
    char signkey_src[256];
    // signKey = md5(hash + 盐 + appid + md5(dfid) + userid)；s_common.mid 即 md5(dfid)。
    // 标准版(非 lite)盐值为 57ae12...；lite 版才是 185672dd...。
    snprintf(signkey_src, sizeof(signkey_src), "%s%s%s%s%s",
             hash, KG_SALT_V5URL_STD, KG_APPID, s_common.mid, s_session.userid);
    kg_md5_hex(signkey_src, strlen(signkey_src), signkey);

    kg_param_t biz[] = {
        { "album_id", "0" }, { "area_code", "1" },
        { "hash", hash }, { "ssa_flag", "is_fromtrack" },
        { "version", KG_V5URL_VERSION }, { "page_id", "151369488" },
        { "quality", "128" }, { "album_audio_id", album_id },
        { "behavior", "play" }, { "pid", "2" }, { "cmd", "26" },
        { "pidversion", "3001" }, { "IsFreePart", "0" },
        { "ppage_id", "463467626,350369493,788954147" },
        { "cdnBackup", "1" }, { "kcard", "0" }, { "module", "" },
        { "clientver", KG_CLIENTVER }, { "key", signkey },
    };
    // /v5/url 需要 encryptKey(key 参数) + Android signature。
    // 参考实现虽写 notSign，但实际仍要 signature（缺了会 20006 err signature）。
    // version 对齐 song_url.js 的 11040；clientver 走公共参数 12569。
    if (kg_request(HOST_GATEWAY, "/v5/url", "GET", biz, 19, 1,
                   NULL, "trackercdn.kugou.com") < 0) {
        ESP_LOGW(TAG, "v5url request failed");
        post_message("播放地址获取失败");
        return;
    }
    ESP_LOGI(TAG, "v5url resp: %.250s", s_resp);
    cJSON *root = cJSON_Parse(s_resp);
    // url 是数组（多 CDN 地址），取第一个；兜底兼容字符串/play_url。
    const char *url = NULL;
    cJSON *url_item = root ? cJSON_GetObjectItemCaseSensitive(root, "url") : NULL;
    if (cJSON_IsString(url_item)) url = url_item->valuestring;
    else if (cJSON_IsArray(url_item) && cJSON_GetArraySize(url_item) > 0) {
        cJSON *first = cJSON_GetArrayItem(url_item, 0);
        if (cJSON_IsString(first)) url = first->valuestring;
    }
    if (!url || !url[0]) url = find_string_field(root, "play_url");
    ESP_LOGI(TAG, "v5url got url=%s", url ? url : "(null)");
    memset(&s_event, 0, sizeof(s_event));
    s_event.type = KG_EV_URL;
    s_event.track_index = cmd->track_index;
    if (url) snprintf(s_event.stream_url, sizeof(s_event.stream_url), "%s", url);
    else snprintf(s_event.message, sizeof(s_event.message), "无播放地址 可能是版权限制");
    cJSON_Delete(root);
    post();
}

// ---------------------------------------------------------------------------
// 校时与启动
// ---------------------------------------------------------------------------

static void sync_time(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    if (esp_netif_sntp_init(&cfg) == ESP_OK) {
        (void)esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
    }
}

static void start_login_flow(void)
{
    char url[KG_QR_URL_CAP];
    if (fetch_qr_key(url, sizeof(url))) {
        post_qr(url);
        s_polling = true;
        post_message("请用酷狗音乐扫码登录");
    } else {
        post_qr(CONFIG_KUGOU_QR_FALLBACK_URL);
        s_polling = false;
        post_message("二维码获取失败 请检查网络");
    }
}

static void after_wifi_ready(void)
{
    sync_time();
    post_wifi(KG_WIFI_READY, NULL, 0, "网络已连接");
    // 先登记设备指纹拿真实 dfid：/v5/url 的 signKey 依赖 mid=md5(dfid)，
    // 占位 "-" 会导致 err signature。失败不影响歌单/曲目，播放时再兜底。
    if (!s_dfid[0]) register_dev();
    if (s_has_session) {
        post_login(KG_LOGIN_DONE);
        memset(&s_event, 0, sizeof(s_event));
        s_event.type = KG_EV_MESSAGE;
        snprintf(s_event.message, sizeof(s_event.message), "已恢复登录态");
        post();
        fetch_playlists();
    } else {
        start_login_flow();
    }
}

static void worker(void *arg)
{
    (void)arg;
    (void)kg_wifi_init();
    session_load();
    dfid_load();

    char ssid[KG_WIFI_SSID_CAP] = "", pass[KG_WIFI_PASS_CAP] = "";
    kg_wifi_load(ssid, sizeof(ssid), pass, sizeof(pass));
    if (ssid[0]) {
        post_wifi(KG_WIFI_CONNECTING, NULL, 0, "正在连接已保存网络");
        if (kg_wifi_connect(ssid, pass) == ESP_OK) after_wifi_ready();
        else post_wifi(KG_WIFI_FAILED, NULL, 0, "网络连接失败 请重新选网");
    } else if (CONFIG_KUGOU_WIFI_SSID[0] != '\0') {
        post_wifi(KG_WIFI_CONNECTING, NULL, 0, "正在连接预置网络");
        if (kg_wifi_connect(CONFIG_KUGOU_WIFI_SSID, CONFIG_KUGOU_WIFI_PASSPHRASE) == ESP_OK) {
            kg_wifi_save(CONFIG_KUGOU_WIFI_SSID, CONFIG_KUGOU_WIFI_PASSPHRASE);
            after_wifi_ready();
        } else {
            post_wifi(KG_WIFI_FAILED, NULL, 0, "预置网络连接失败");
        }
    } else {
        post_wifi(KG_WIFI_IDLE, NULL, 0, "请先选择 Wi-Fi 网络");
    }

    kg_cmd_t cmd;
    while (1) {
        TickType_t wait = s_polling ? pdMS_TO_TICKS(POLL_MS) : portMAX_DELAY;
        if (xQueueReceive(s_cmd_q, &cmd, wait) == pdTRUE) {
            switch (cmd.type) {
            case KG_CMD_SCAN: {
                kg_ap_t aps[KG_WIFI_AP_MAX];
                uint8_t count = 0;
                if (kg_wifi_scan(aps, &count, KG_WIFI_AP_MAX) == ESP_OK) {
                    post_wifi(KG_WIFI_IDLE, aps, count, "上下选择 确认连接");
                } else {
                    post_wifi(KG_WIFI_FAILED, NULL, 0, "扫描失败 请重试");
                }
                break;
            }
            case KG_CMD_CONNECT:
                post_wifi(KG_WIFI_CONNECTING, NULL, 0, "正在连接网络");
                if (kg_wifi_connect(cmd.ssid, cmd.pass) == ESP_OK) {
                    kg_wifi_save(cmd.ssid, cmd.pass);
                    after_wifi_ready();
                } else {
                    post_wifi(KG_WIFI_FAILED, NULL, 0, "连接失败 请检查密码");
                }
                break;
            case KG_CMD_QR_START:
                if (kg_wifi_ready()) start_login_flow();
                break;
            case KG_CMD_QR_REFRESH:
                s_polling = false;
                if (kg_wifi_ready()) start_login_flow();
                else post_message("请先配置网络");
                break;
            case KG_CMD_PLAYLISTS:
                if (s_has_session) fetch_playlists();
                break;
            case KG_CMD_TRACKS:
                if (s_has_session) fetch_tracks(&cmd);
                break;
            case KG_CMD_TRACK_URL:
                if (s_has_session) fetch_track_url(&cmd);
                break;
            case KG_CMD_LOGOUT:
                session_clear();
                s_polling = false;
                post_login(KG_LOGIN_WAIT);
                start_login_flow();
                break;
            default: break;
            }
            continue;
        }
        if (!s_polling) continue;

        kg_login_t state = poll_qr();
        post_login(state);
        if (state == KG_LOGIN_DONE) {
            s_polling = false;
            fetch_playlists();
        } else if (state == KG_LOGIN_EXPIRED) {
            char url[KG_QR_URL_CAP];
            if (fetch_qr_key(url, sizeof(url))) post_qr(url);
        } else if (state == KG_LOGIN_ERROR) {
            s_polling = false;
            post_message("登录接口异常 请长按确认重试");
        }
    }
}

esp_err_t kg_api_start(kg_event_cb_t cb, void *user)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    s_cb = cb;
    s_user = user;
    s_cmd_q = xQueueCreate(4, sizeof(kg_cmd_t));
    if (!s_cmd_q) return ESP_ERR_NO_MEM;
    // 栈必须给足 16KB：fetch_playlists 走 HTTPS POST + cJSON 解析，TLS 握手后
    // 仍会调用 newlib 的 snprintf（vfprintf 栈消耗大），12KB 会栈溢出 panic。
    if (xTaskCreate(worker, "kg_net", 16384, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void kg_api_send(const kg_cmd_t *cmd)
{
    if (s_cmd_q && cmd) (void)xQueueSend(s_cmd_q, cmd, 0);
}
