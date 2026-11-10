// main/kugou_keyboard.c —— 见 kugou_keyboard.h。
#include "kugou_keyboard.h"

#include <stdio.h>
#include <string.h>

// 四组覆盖可打印 ASCII，组内不重复；符号组含空格，便于输含空格的密码。
static const char *const k_groups[KG_KB_GROUPS] = {
    "abcdefghijklmnopqrstuvwxyz",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "0123456789",
    " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~",
};

static const char *const k_group_names[KG_KB_GROUPS] = {
    "小写", "大写", "数字", "符号",
};

const char *kg_keyboard_chars(uint8_t group)
{
    return k_groups[group % KG_KB_GROUPS];
}

const char *kg_keyboard_group_name(uint8_t group)
{
    return k_group_names[group % KG_KB_GROUPS];
}

void kg_keyboard_init(kg_keyboard_t *kb)
{
    if (!kb) return;
    memset(kb, 0, sizeof(*kb));
}

char kg_keyboard_current(const kg_keyboard_t *kb)
{
    if (!kb) return '\0';
    const char *chars = k_groups[kb->group % KG_KB_GROUPS];
    size_t len = strlen(chars);
    if (len == 0) return '\0';
    return chars[kb->index % len];
}

kg_kb_action_t kg_keyboard_key(kg_keyboard_t *kb, kg_key_t key, kg_key_ev_t ev)
{
    if (!kb) return KG_KB_NONE;
    const char *chars = k_groups[kb->group % KG_KB_GROUPS];
    size_t len = strlen(chars);

    if (ev == KG_KEY_LONG) {
        switch (key) {
        case KG_KEY_UP:    // 长按上：切换到上一组
            kb->group = (uint8_t)((kb->group + KG_KB_GROUPS - 1) % KG_KB_GROUPS);
            kb->index = 0;
            return KG_KB_CHANGED;
        case KG_KEY_DOWN:  // 长按下：删除末尾字符
            {
                size_t n = strlen(kb->text);
                if (n > 0) {
                    kb->text[n - 1] = '\0';
                    return KG_KB_CHANGED;
                }
            }
            return KG_KB_NONE;  // 密码已空：无操作
        case KG_KEY_OK:    // 长按确认：有密码提交，空密码取消(返回配网页)
            return (strlen(kb->text) > 0) ? KG_KB_SUBMIT : KG_KB_CANCEL;
        default:
            return KG_KB_NONE;
        }
    }

    switch (key) {
    case KG_KEY_UP:
        kb->index = (uint8_t)((kb->index + len - 1) % len);
        return KG_KB_CHANGED;
    case KG_KEY_DOWN:
        kb->index = (uint8_t)((kb->index + 1) % len);
        return KG_KB_CHANGED;
    case KG_KEY_OK:
        {
            size_t n = strlen(kb->text);
            if (n + 2 <= KG_KB_TEXT_CAP) {  // 最多 63 字符 + NUL
                kb->text[n] = kg_keyboard_current(kb);
                kb->text[n + 1] = '\0';
            }
        }
        return KG_KB_CHANGED;
    default:
        return KG_KB_NONE;
    }
}
