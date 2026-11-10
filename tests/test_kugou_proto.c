// tests/test_kugou_proto.c —— 扫码登录 URL 与 JSON 解析的主机测试。
#include <stdio.h>
#include <string.h>

#include "kugou_proto.h"

static int failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static void test_url(void)
{
    char out[KG_QR_URL_CAP];
    const char *tpl = "https://h5.kugou.com/apps/loginQRCode/html/index.html?qrcode=%s";

    int n = kg_proto_build_login_url(out, sizeof(out), tpl, "abc123");
    CHECK(n > 0);
    CHECK(strcmp(out, "https://h5.kugou.com/apps/loginQRCode/html/index.html?qrcode=abc123") == 0);

    // 模板里没有 %s：原样输出
    CHECK(kg_proto_build_login_url(out, sizeof(out), "https://example.com/q", "k") > 0);
    CHECK(strcmp(out, "https://example.com/q") == 0);

    // 空 key 生成空参数也是合法 URL
    CHECK(kg_proto_build_login_url(out, sizeof(out), "u?k=%s", "") > 0);
    CHECK(strcmp(out, "u?k=") == 0);

    // 缓冲区不足必须失败，不能截断出一个半截 URL
    char tiny[8];
    CHECK(kg_proto_build_login_url(tiny, sizeof(tiny), tpl, "abc123") == -1);
    CHECK(kg_proto_build_login_url(NULL, 0, tpl, "x") == -1);
    CHECK(kg_proto_build_login_url(out, sizeof(out), NULL, "x") == -1);
    CHECK(kg_proto_build_login_url(out, sizeof(out), tpl, NULL) == -1);

    // 恰好装得下
    char exact[6];
    CHECK(kg_proto_build_login_url(exact, sizeof(exact), "a=%s", "xyz") == 5);
    CHECK(strcmp(exact, "a=xyz") == 0);
}

static void test_json_string(void)
{
    char out[KG_QR_KEY_CAP];
    CHECK(kg_proto_extract_string("{\"data\":{\"qrcode\":\"KEY-8899\"}}",
                                  "qrcode", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "KEY-8899") == 0);

    CHECK(kg_proto_extract_string("{ \"status\" : 1 , \"key\" : \"a b\" }",
                                  "key", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "a b") == 0);

    CHECK(kg_proto_extract_string("{\"data\":{\"qrcode\":\"\"}}",
                                  "qrcode", out, sizeof(out)) == 0);
    CHECK(out[0] == '\0');

    CHECK(kg_proto_extract_string("{\"data\":{\"qrcode\":\"a\\nb\"}}",
                                  "qrcode", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "a\nb") == 0);

    CHECK(kg_proto_extract_string("{\"other\":1}", "qrcode", out, sizeof(out)) == -1);
    CHECK(kg_proto_extract_string("{\"qrcode\":42}", "qrcode", out, sizeof(out)) == -1);
    CHECK(kg_proto_extract_string("{\"qrcode\":\"unterminated}",
                                  "qrcode", out, sizeof(out)) == -1);
    CHECK(out[0] == '\0');

    char tiny[3];
    CHECK(kg_proto_extract_string("{\"qrcode\":\"abcdef\"}",
                                  "qrcode", tiny, sizeof(tiny)) == -1);
}

static void test_json_int(void)
{
    int value = -1;
    CHECK(kg_proto_extract_int("{\"data\":{\"status\":4}}", "status", &value) == 0);
    CHECK(value == 4);
    CHECK(kg_proto_extract_int("{\"status\" : 801 , \"x\":9}", "status", &value) == 0);
    CHECK(value == 801);
    CHECK(kg_proto_extract_int("{\"status\":-2}", "status", &value) == 0);
    CHECK(value == -2);
    CHECK(kg_proto_extract_int("{\"status\":\"4\"}", "status", &value) == -1);
    CHECK(kg_proto_extract_int("{\"other\":1}", "status", &value) == -1);
    CHECK(kg_proto_extract_int(NULL, "status", &value) == -1);
    CHECK(kg_proto_extract_int("{\"status\":1}", "status", NULL) == -1);
}

static void test_login_state(void)
{
    CHECK(kg_proto_login_state(0) == KG_LOGIN_EXPIRED);
    CHECK(kg_proto_login_state(800) == KG_LOGIN_EXPIRED);
    CHECK(kg_proto_login_state(1) == KG_LOGIN_WAIT);
    CHECK(kg_proto_login_state(801) == KG_LOGIN_WAIT);
    CHECK(kg_proto_login_state(2) == KG_LOGIN_SCANNED);
    CHECK(kg_proto_login_state(802) == KG_LOGIN_SCANNED);
    CHECK(kg_proto_login_state(4) == KG_LOGIN_DONE);
    CHECK(kg_proto_login_state(803) == KG_LOGIN_DONE);
    CHECK(kg_proto_login_state(999) == KG_LOGIN_ERROR);
    CHECK(kg_proto_login_state(-1) == KG_LOGIN_ERROR);
}

int main(void)
{
    test_url();
    test_json_string();
    test_json_int();
    test_login_state();
    if (failures) {
        printf("kugou proto: FAIL (%d)\n", failures);
        return 1;
    }
    printf("kugou proto: PASS\n");
    return 0;
}
