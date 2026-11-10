// main/kugou_proto.c —— 见 kugou_proto.h。
#include "kugou_proto.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int kg_proto_build_login_url(char *out, size_t cap,
                             const char *templ, const char *key)
{
    if (!out || cap == 0 || !templ || !key) return -1;
    size_t used = 0;
    out[0] = '\0';
    for (const char *p = templ; *p; ) {
        const char *chunk = NULL;
        size_t len = 0;
        if (p[0] == '%' && p[1] == 's') {
            chunk = key;
            len = strlen(key);
            p += 2;
        } else {
            chunk = p;
            len = 1;
            p += 1;
        }
        if (used + len + 1 > cap) return -1;
        memcpy(out + used, chunk, len);
        used += len;
    }
    out[used] = '\0';
    return (int)used;
}

// 找到 "field" 之后的值起始位置（跳过冒号与空白）。
static const char *find_value(const char *json, const char *field)
{
    if (!json || !field || !*field) return NULL;
    char pattern[64];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\"", field);
    if (n < 0 || (size_t)n >= sizeof(pattern)) return NULL;
    const char *hit = strstr(json, pattern);
    if (!hit) return NULL;
    const char *p = hit + (size_t)n;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ':') return NULL;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

int kg_proto_extract_string(const char *json, const char *field,
                            char *out, size_t cap)
{
    if (!out || cap == 0) return -1;
    out[0] = '\0';
    const char *p = find_value(json, field);
    if (!p || *p != '"') return -1;
    p++;
    size_t used = 0;
    while (*p && *p != '"') {
        char ch = *p;
        if (ch == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': ch = '\n'; break;
            case 't': ch = '\t'; break;
            case 'r': ch = '\r'; break;
            default:  ch = *p; break;
            }
        }
        if (used + 2 > cap) {
            out[0] = '\0';  // 失败时不留下截断的半个字符串
            return -1;
        }
        out[used++] = ch;
        p++;
    }
    if (*p != '"') {
        out[0] = '\0';
        return -1;
    }
    out[used] = '\0';
    return 0;
}

int kg_proto_extract_int(const char *json, const char *field, int *out)
{
    if (!out) return -1;
    const char *p = find_value(json, field);
    if (!p || (*p != '-' && !isdigit((unsigned char)*p))) return -1;
    char *end = NULL;
    long value = strtol(p, &end, 10);
    if (end == p) return -1;
    *out = (int)value;
    return 0;
}

kg_login_t kg_proto_login_state(int status)
{
    switch (status) {
    case 0:
    case 800: return KG_LOGIN_EXPIRED;
    case 1:
    case 801: return KG_LOGIN_WAIT;
    case 2:
    case 802: return KG_LOGIN_SCANNED;
    case 4:
    case 803: return KG_LOGIN_DONE;
    default:  return KG_LOGIN_ERROR;
    }
}
