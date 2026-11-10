// tests/test_kugou_sign.c —— 酷狗签名与 URL 构造的主机测试。
//
// 期望值由 MIT 许可的 KuGouMusicApi（util/helper.js、util/request.js）的
// 参考算法在 Python 侧逐字节复算得到，防止设备端 C 实现与协议行为漂移。
#include <stdio.h>
#include <string.h>

#include "kugou_sign.h"

static int failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            failures++;                                                        \
        }                                                                      \
    } while (0)

#define CHECK_STR(got, want)                                                   \
    do {                                                                       \
        if (strcmp((got), (want)) != 0) {                                      \
            printf("FAIL %s:%d: got [%s] want [%s]\n",                         \
                   __FILE__, __LINE__, (got), (want));                          \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static void test_md5(void)
{
    char out[KG_MD5_HEX_CAP];
    kg_md5_hex("", 0, out);
    CHECK_STR(out, "d41d8cd98f00b204e9800998ecf8427e");
    kg_md5_hex("abc", 3, out);
    CHECK_STR(out, "900150983cd24fb0d6963f7d28e17f72");
    kg_md5_hex("The quick brown fox jumps over the lazy dog", 43, out);
    CHECK_STR(out, "9e107d9d372bb6826bd81d3542a419d6");
    kg_md5_hex("-", 1, out);
    CHECK_STR(out, "336d5ebc5436534e61d16e63ddfca327");
    // 跨块长度（>55 字节会走两个分组）
    const char *long_text = "0123456789012345678901234567890123456789012345678901234567890123456789";
    kg_md5_hex(long_text, strlen(long_text), out);
    CHECK(strlen(out) == 32);
}

static void test_common(void)
{
    kg_common_t c;
    kg_common_init(&c, NULL, NULL, NULL, 1700000000u);
    CHECK_STR(c.dfid, "-");
    CHECK_STR(c.mid, "336d5ebc5436534e61d16e63ddfca327");
    CHECK_STR(c.uuid, "15e772e1213bdd0718d0c1d10d64e06f");
    CHECK_STR(c.clienttime, "1700000000");
    CHECK_STR(c.userid, "0");
    CHECK(c.token[0] == '\0');

    kg_common_init(&c, "device-abc", "123", "TOKEN-X", 1700000001u);
    CHECK_STR(c.userid, "123");
    CHECK_STR(c.token, "TOKEN-X");

    kg_param_t params[KG_PARAM_MAX];
    size_t n = kg_common_params(&c, params, KG_PARAM_MAX);
    CHECK(n == 8);  // 含 token
    kg_common_init(&c, "-", "0", "", 1u);
    n = kg_common_params(&c, params, KG_PARAM_MAX);
    CHECK(n == 7);  // 无 token
}

static void test_web_signature(void)
{
    kg_common_t c;
    kg_common_init(&c, "-", "0", "", 1700000000u);
    kg_param_t p[KG_PARAM_MAX];
    size_t n = kg_common_params(&c, p, KG_PARAM_MAX);
    // 登录 QR 接口会覆盖公共 appid，并附加业务参数
    for (size_t i = 0; i < n; i++) {
        if (strcmp(p[i].key, "appid") == 0) p[i].value = KG_LOGIN_APPID;
    }
    p[n++] = (kg_param_t){ "type", "1" };
    p[n++] = (kg_param_t){ "plat", "4" };
    p[n++] = (kg_param_t){ "srcappid", KG_SRCAPPID };
    p[n++] = (kg_param_t){ "qrcode_txt",
        "https://h5.kugou.com/apps/loginQRCode/html/index.html?appid=1005&" };

    char joined[2048];
    CHECK(kg_sign_join(p, n, joined, sizeof(joined)) > 0);
    CHECK_STR(joined,
        "appid=1001clienttime=1700000000clientver=12569dfid=-"
        "mid=336d5ebc5436534e61d16e63ddfca327plat=4"
        "qrcode_txt=https://h5.kugou.com/apps/loginQRCode/html/index.html?appid=1005&"
        "srcappid=2919type=1userid=0uuid=15e772e1213bdd0718d0c1d10d64e06f");

    char sig[KG_MD5_HEX_CAP];
    kg_sign_web(p, n, sig);
    CHECK_STR(sig, "f449c35b8a9ee82b5d833f01c9f3384e");
}

static void test_android_signature(void)
{
    kg_common_t c;
    kg_common_init(&c, "-", "123", "abcDEF", 1700000000u);
    kg_param_t p[KG_PARAM_MAX];
    size_t n = kg_common_params(&c, p, KG_PARAM_MAX);
    p[n++] = (kg_param_t){ "page", "1" };
    p[n++] = (kg_param_t){ "pagesize", "30" };

    char joined[2048];
    CHECK(kg_sign_join_by_key(p, n, joined, sizeof(joined)) > 0);
    CHECK_STR(joined,
        "appid=1005clienttime=1700000000clientver=12569dfid=-"
        "mid=336d5ebc5436534e61d16e63ddfca327page=1pagesize=30"
        "token=abcDEFuserid=123uuid=15e772e1213bdd0718d0c1d10d64e06f");

    const char *body =
        "{\"userid\":\"123\",\"token\":\"abcDEF\",\"total_ver\":979,"
        "\"type\":2,\"page\":1,\"pagesize\":30}";
    char sig[KG_MD5_HEX_CAP];
    kg_sign_android(p, n, body, sig);
    CHECK_STR(sig, "0cf46b68fe0979d46db2ce087b717aad");
    // body 参与签名，空 body 必须得到不同结果
    kg_sign_android(p, n, NULL, sig);
    CHECK_STR(sig, "bb5354dd22b4edb2ede60b051826749b");
}

static void test_register_signature(void)
{
    kg_common_t c;
    kg_common_init(&c, "-", "0", "", 1700000000u);
    kg_param_t p[KG_PARAM_MAX];
    size_t n = kg_common_params(&c, p, KG_PARAM_MAX);
    char sig[KG_MD5_HEX_CAP];
    kg_sign_register(p, n, sig);
    // 参考实现：md5("1014" + 七个公共参数的 value 排序拼接 + "1014")
    CHECK_STR(sig, "4a9f483b0d75ad771d12e497f4ddc1e3");
}

static void test_url(void)
{
    char enc[256];
    CHECK(kg_url_encode("a b&c=d?", enc, sizeof(enc)) > 0);
    CHECK_STR(enc, "a%20b%26c%3Dd%3F");
    CHECK(kg_url_encode("AZaz09-_.~", enc, sizeof(enc)) > 0);
    CHECK_STR(enc, "AZaz09-_.~");
    CHECK(kg_url_encode("x", enc, 1) == -1);
    CHECK(kg_url_encode("x", enc, 2) == 1);

    kg_param_t p[2] = { { "a", "1" }, { "b", "a b" } };
    char url[512];
    int n = kg_build_url(url, sizeof(url), "https://gateway.kugou.com",
                         "/v2/qrcode", p, 2, "DEADBEEF");
    CHECK(n > 0);
    CHECK_STR(url, "https://gateway.kugou.com/v2/qrcode?a=1&b=a%20b&signature=DEADBEEF");

    CHECK(kg_build_url(url, sizeof(url), "https://x", "/p", NULL, 0, NULL) > 0);
    CHECK_STR(url, "https://x/p?");
}

int main(void)
{
    test_md5();
    test_common();
    test_web_signature();
    test_android_signature();
    test_register_signature();
    test_url();
    if (failures) {
        printf("kugou sign: FAIL (%d)\n", failures);
        return 1;
    }
    printf("kugou sign: PASS\n");
    return 0;
}
