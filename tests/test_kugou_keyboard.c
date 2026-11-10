// tests/test_kugou_keyboard.c —— 配网屏幕键盘纯逻辑测试。
#include <stdio.h>
#include <string.h>

#include "kugou_keyboard.h"
#include "kugou_model.h"

static int failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            failures++;                                                        \
        }                                                                      \
    } while (0)

int main(void)
{
    kg_keyboard_t kb;
    kg_keyboard_init(&kb);
    CHECK(strlen(kb.text) == 0);
    CHECK(kb.group == 0);
    CHECK(kb.index == 0);
    CHECK(kg_keyboard_current(&kb) == 'a');
    CHECK(strcmp(kg_keyboard_group_name(0), "小写") == 0);

    // 上下移动字符，循环
    CHECK(kg_keyboard_key(&kb, KG_KEY_DOWN, KG_KEY_CLICK) == KG_KB_CHANGED);
    CHECK(kg_keyboard_current(&kb) == 'b');
    CHECK(kg_keyboard_key(&kb, KG_KEY_UP, KG_KEY_CLICK) == KG_KB_CHANGED);
    CHECK(kg_keyboard_current(&kb) == 'a');
    CHECK(kg_keyboard_key(&kb, KG_KEY_UP, KG_KEY_CLICK) == KG_KB_CHANGED);
    CHECK(kg_keyboard_current(&kb) == 'z');

    // 输入字符
    kg_keyboard_init(&kb);
    CHECK(kg_keyboard_key(&kb, KG_KEY_OK, KG_KEY_CLICK) == KG_KB_CHANGED);
    CHECK(strcmp(kb.text, "a") == 0);
    kg_keyboard_key(&kb, KG_KEY_DOWN, KG_KEY_CLICK);
    kg_keyboard_key(&kb, KG_KEY_OK, KG_KEY_CLICK);
    CHECK(strcmp(kb.text, "ab") == 0);

    // 长按上换组，组切换后索引归零
    CHECK(kg_keyboard_key(&kb, KG_KEY_UP, KG_KEY_LONG) == KG_KB_CHANGED);
    CHECK(kb.group == 3);  // 从 0 回绕到符号组
    CHECK(kb.index == 0);
    CHECK(kg_keyboard_current(&kb) == ' ');

    // 长按下删除
    CHECK(kg_keyboard_key(&kb, KG_KEY_DOWN, KG_KEY_LONG) == KG_KB_CHANGED);
    CHECK(strcmp(kb.text, "a") == 0);

    // 长按确认提交
    CHECK(kg_keyboard_key(&kb, KG_KEY_OK, KG_KEY_LONG) == KG_KB_SUBMIT);
    CHECK(strcmp(kb.text, "a") == 0);  // 提交本身不改文本

    // 缓冲区上限
    kg_keyboard_init(&kb);
    for (int i = 0; i < KG_KB_TEXT_CAP + 10; i++) {
        kg_keyboard_key(&kb, KG_KEY_OK, KG_KEY_CLICK);
    }
    CHECK(strlen(kb.text) == KG_KB_TEXT_CAP - 1);

    // 四组都不为空且首字符不同
    CHECK(strlen(kg_keyboard_chars(0)) == 26);
    CHECK(strlen(kg_keyboard_chars(1)) == 26);
    CHECK(strlen(kg_keyboard_chars(2)) == 10);
    CHECK(strlen(kg_keyboard_chars(3)) > 10);

    if (failures) {
        printf("kugou keyboard: FAIL (%d)\n", failures);
        return 1;
    }
    printf("kugou keyboard: PASS\n");
    return 0;
}
