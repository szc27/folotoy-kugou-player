// main/kugou_sign.h —— 酷狗私有协议中与平台无关的部分：MD5、请求签名、URL 构造。
//
// 协议行为参考 MIT 许可的 KuGouMusicApi（280867678/KuGouMusicApi）2026-11 版本，
// 这里按协议行为独立实现，不复制其源码；盐值与公共参数均为该客户端公开的固定值。
// 本文件不依赖 ESP-IDF / mbedTLS，便于在 PC 上用参考实现的输出做逐字节对拍。
#pragma once

#include <stddef.h>
#include <stdint.h>

#define KG_SALT_WEB      "NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt"
#define KG_SALT_ANDROID  "OIlwieks28dk2k092lksi2UIkp"
#define KG_APPID         "1005"
#define KG_CLIENTVER     "12569"
#define KG_SRCAPPID      "2919"
#define KG_LOGIN_APPID   "1001"
#define KG_MD5_HEX_CAP   33
#define KG_PARAM_MAX     32

typedef struct {
    const char *key;
    const char *value;
} kg_param_t;

// 标准 MD5，输出 32 位小写十六进制 + NUL。
void kg_md5_hex(const void *data, size_t len, char out[KG_MD5_HEX_CAP]);

// 公共参数：dfid -> mid = md5(dfid)，uuid = md5(dfid + mid)。
typedef struct {
    char dfid[64];
    char mid[KG_MD5_HEX_CAP];
    char uuid[KG_MD5_HEX_CAP];
    char clienttime[24];
    char userid[24];
    char token[256];
} kg_common_t;

void kg_common_init(kg_common_t *c, const char *dfid,
                    const char *userid, const char *token, uint32_t unix_seconds);

// 把公共参数填进参数表（不含业务参数），返回已用个数。
size_t kg_common_params(const kg_common_t *c, kg_param_t *out, size_t cap);

// Web 签名按 "k=v" 字符串整体字典序；Android 签名按 key 字典序。
// 两者在多数参数集上等价，但协议定义不同，故分别实现。
int kg_sign_join(kg_param_t *params, size_t count, char *out, size_t cap);
int kg_sign_join_by_key(kg_param_t *params, size_t count, char *out, size_t cap);

// web:   md5(salt + joined + salt)
// android: md5(salt + joined + body + salt)，body 为 POST 的 JSON 文本或空串
// register: md5("1014" + sorted values + "1014")
void kg_sign_web(kg_param_t *params, size_t count, char out[KG_MD5_HEX_CAP]);
void kg_sign_android(kg_param_t *params, size_t count, const char *body,
                     char out[KG_MD5_HEX_CAP]);
void kg_sign_register(kg_param_t *params, size_t count, char out[KG_MD5_HEX_CAP]);

// 组装带签名的完整 URL（自动对 value 做 percent-encode）；返回长度或 -1。
int kg_build_url(char *out, size_t cap, const char *base, const char *path,
                 kg_param_t *params, size_t count, const char *signature);

// 单个字符串的 percent-encode（保留 unreserved 字符）。
int kg_url_encode(const char *src, char *out, size_t cap);
