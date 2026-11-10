// main/kugou_sign.c —— 见 kugou_sign.h。
//
// MD5 为按 RFC 1321 独立实现的紧凑版本，固件与主机测试共用同一份代码，
// 避免两侧算法实现不同导致"主机测过、真机签名错"。
#include "kugou_sign.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// MD5 (RFC 1321)
// ---------------------------------------------------------------------------

static const uint32_t k_md5_k[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
};

static const uint32_t k_md5_s[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static uint32_t rotl32(uint32_t x, uint32_t n)
{
    return (x << n) | (x >> (32 - n));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void md5_block(uint32_t state[4], const uint8_t *block)
{
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t m[16];
    for (int i = 0; i < 16; i++) m[i] = le32(block + i * 4);

    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        uint32_t tmp = d;
        d = c;
        c = b;
        b = b + rotl32(a + f + k_md5_k[i] + m[g], k_md5_s[i]);
        a = tmp;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

void kg_md5_hex(const void *data, size_t len, char out[KG_MD5_HEX_CAP])
{
    static const char hex[] = "0123456789abcdef";
    size_t padded = ((len + 8) / 64 + 1) * 64;
    uint8_t *buf = (uint8_t *)calloc(1, padded);
    if (!buf) { out[0] = '\0'; return; }
    if (data && len) memcpy(buf, data, len);
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8u;
    for (int i = 0; i < 8; i++) buf[padded - 8 + i] = (uint8_t)(bits >> (8 * i));

    uint32_t state[4] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u };
    for (size_t off = 0; off < padded; off += 64) md5_block(state, buf + off);
    free(buf);

    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            uint8_t byte = (uint8_t)(state[i] >> (8 * j));
            out[i * 8 + j * 2] = hex[byte >> 4];
            out[i * 8 + j * 2 + 1] = hex[byte & 0x0f];
        }
    }
    out[32] = '\0';
}

// ---------------------------------------------------------------------------
// 公共参数
// ---------------------------------------------------------------------------

void kg_common_init(kg_common_t *c, const char *dfid,
                    const char *userid, const char *token, uint32_t unix_seconds)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    snprintf(c->dfid, sizeof(c->dfid), "%s", (dfid && dfid[0]) ? dfid : "-");
    kg_md5_hex(c->dfid, strlen(c->dfid), c->mid);
    char pair[128];
    snprintf(pair, sizeof(pair), "%s%s", c->dfid, c->mid);
    kg_md5_hex(pair, strlen(pair), c->uuid);
    snprintf(c->clienttime, sizeof(c->clienttime), "%lu", (unsigned long)unix_seconds);
    snprintf(c->userid, sizeof(c->userid), "%s", (userid && userid[0]) ? userid : "0");
    snprintf(c->token, sizeof(c->token), "%s", token ? token : "");
}

size_t kg_common_params(const kg_common_t *c, kg_param_t *out, size_t cap)
{
    if (!c || !out) return 0;
    size_t n = 0;
#define ADD(K, V) do { if (n < cap) { out[n].key = (K); out[n].value = (V); n++; } } while (0)
    ADD("dfid", c->dfid);
    ADD("mid", c->mid);
    ADD("uuid", c->uuid);
    ADD("appid", KG_APPID);
    ADD("clientver", KG_CLIENTVER);
    ADD("userid", c->userid);
    ADD("clienttime", c->clienttime);
    if (c->token[0]) ADD("token", c->token);
#undef ADD
    return n;
}

// ---------------------------------------------------------------------------
// 签名
// ---------------------------------------------------------------------------

#define KG_ITEM_CAP 384

static void sort_strings(char items[][KG_ITEM_CAP], size_t count)
{
    for (size_t i = 1; i < count; i++) {
        char tmp[KG_ITEM_CAP];
        memcpy(tmp, items[i], KG_ITEM_CAP);
        size_t j = i;
        while (j > 0 && strcmp(items[j - 1], tmp) > 0) {
            memcpy(items[j], items[j - 1], KG_ITEM_CAP);
            j--;
        }
        memcpy(items[j], tmp, KG_ITEM_CAP);
    }
}

int kg_sign_join(kg_param_t *params, size_t count, char *out, size_t cap)
{
    if (!params || !out || cap == 0 || count > KG_PARAM_MAX) return -1;
    char (*items)[KG_ITEM_CAP] = (char (*)[KG_ITEM_CAP])calloc(count, KG_ITEM_CAP);
    if (!items) return -1;
    for (size_t i = 0; i < count; i++) {
        snprintf(items[i], KG_ITEM_CAP, "%s=%s",
                 params[i].key ? params[i].key : "",
                 params[i].value ? params[i].value : "");
    }
    sort_strings(items, count);
    size_t used = 0;
    out[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        size_t n = strlen(items[i]);
        if (used + n + 1 > cap) { free(items); return -1; }
        memcpy(out + used, items[i], n);
        used += n;
    }
    out[used] = '\0';
    free(items);
    return (int)used;
}

int kg_sign_join_by_key(kg_param_t *params, size_t count, char *out, size_t cap)
{
    if (!params || !out || cap == 0 || count > KG_PARAM_MAX) return -1;
    // 插入排序按 key 字典序（JS: Object.keys(params).sort()）
    for (size_t i = 1; i < count; i++) {
        kg_param_t tmp = params[i];
        size_t j = i;
        while (j > 0 && strcmp(params[j - 1].key, tmp.key) > 0) {
            params[j] = params[j - 1];
            j--;
        }
        params[j] = tmp;
    }
    size_t used = 0;
    out[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        const char *k = params[i].key ? params[i].key : "";
        const char *v = params[i].value ? params[i].value : "";
        size_t n = strlen(k) + strlen(v) + 1;
        if (used + n + 1 > cap) return -1;
        int w = snprintf(out + used, cap - used, "%s=%s", k, v);
        if (w < 0) return -1;
        used += (size_t)w;
    }
    return (int)used;
}

static void sign_with_salt(kg_param_t *params, size_t count,
                           const char *salt, const char *body, int by_key,
                           char out[KG_MD5_HEX_CAP])
{
    size_t cap = 2048;
    char *joined = (char *)malloc(cap);
    char *plain = (char *)malloc(cap + 512);
    if (!joined || !plain) { free(joined); free(plain); out[0] = '\0'; return; }
    int ok = by_key ? kg_sign_join_by_key(params, count, joined, cap)
                    : kg_sign_join(params, count, joined, cap);
    if (ok < 0) { free(joined); free(plain); out[0] = '\0'; return; }
    snprintf(plain, cap + 512, "%s%s%s%s", salt, joined, body ? body : "", salt);
    kg_md5_hex(plain, strlen(plain), out);
    free(joined);
    free(plain);
}

void kg_sign_web(kg_param_t *params, size_t count, char out[KG_MD5_HEX_CAP])
{
    sign_with_salt(params, count, KG_SALT_WEB, NULL, 0, out);
}

void kg_sign_android(kg_param_t *params, size_t count, const char *body,
                     char out[KG_MD5_HEX_CAP])
{
    sign_with_salt(params, count, KG_SALT_ANDROID, body, 1, out);
}

void kg_sign_register(kg_param_t *params, size_t count, char out[KG_MD5_HEX_CAP])
{
    char (*items)[KG_ITEM_CAP] = (char (*)[KG_ITEM_CAP])calloc(count ? count : 1, KG_ITEM_CAP);
    if (!items) { out[0] = '\0'; return; }
    for (size_t i = 0; i < count; i++) {
        snprintf(items[i], KG_ITEM_CAP, "%s", params[i].value ? params[i].value : "");
    }
    sort_strings(items, count);
    size_t cap = 2048;
    char *plain = (char *)malloc(cap);
    if (!plain) { free(items); out[0] = '\0'; return; }
    size_t used = (size_t)snprintf(plain, cap, "1014");
    for (size_t i = 0; i < count; i++) {
        size_t n = strlen(items[i]);
        if (used + n + 8 >= cap) break;
        memcpy(plain + used, items[i], n);
        used += n;
    }
    memcpy(plain + used, "1014", 5);
    kg_md5_hex(plain, strlen(plain), out);
    free(plain);
    free(items);
}

// ---------------------------------------------------------------------------
// URL
// ---------------------------------------------------------------------------

int kg_url_encode(const char *src, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!src || !out || cap == 0) return -1;
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        unsigned char ch = *p;
        int safe = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                   (ch >= '0' && ch <= '9') ||
                   ch == '-' || ch == '_' || ch == '.' || ch == '~';
        size_t need = safe ? 1 : 3;
        if (used + need + 1 > cap) return -1;
        if (safe) out[used++] = (char)ch;
        else {
            out[used++] = '%';
            out[used++] = hex[ch >> 4];
            out[used++] = hex[ch & 0x0f];
        }
    }
    out[used] = '\0';
    return (int)used;
}

int kg_build_url(char *out, size_t cap, const char *base, const char *path,
                 kg_param_t *params, size_t count, const char *signature)
{
    if (!out || cap == 0 || !base || !path) return -1;
    size_t used = (size_t)snprintf(out, cap, "%s%s?", base, path);
    if (used >= cap) return -1;
    char encoded[1024];
    for (size_t i = 0; i < count; i++) {
        if (kg_url_encode(params[i].value ? params[i].value : "",
                          encoded, sizeof(encoded)) < 0) return -1;
        int n = snprintf(out + used, cap - used, "%s%s=%s",
                         i ? "&" : "",
                         params[i].key ? params[i].key : "", encoded);
        if (n < 0 || (size_t)n >= cap - used) return -1;
        used += (size_t)n;
    }
    if (signature) {
        int n = snprintf(out + used, cap - used, "%ssignature=%s",
                         count ? "&" : "", signature);
        if (n < 0 || (size_t)n >= cap - used) return -1;
        used += (size_t)n;
    }
    return (int)used;
}
