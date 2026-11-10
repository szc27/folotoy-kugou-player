// main/kugou_keyboard.h —— 三键屏幕键盘的纯逻辑（配网输密码用）。
// 不依赖 ESP-IDF / LVGL，可主机测试。
#pragma once

#include <stdint.h>

#include "kugou_model.h"

#define KG_KB_TEXT_CAP 64
#define KG_KB_GROUPS   4

typedef enum {
    KG_KB_NONE = 0,
    KG_KB_CHANGED,
    KG_KB_SUBMIT,
    KG_KB_CANCEL,
} kg_kb_action_t;

typedef struct {
    char text[KG_KB_TEXT_CAP];
    uint8_t group;
    uint8_t index;
} kg_keyboard_t;

void kg_keyboard_init(kg_keyboard_t *kb);
kg_kb_action_t kg_keyboard_key(kg_keyboard_t *kb, kg_key_t key, kg_key_ev_t ev);
const char *kg_keyboard_chars(uint8_t group);
const char *kg_keyboard_group_name(uint8_t group);
char kg_keyboard_current(const kg_keyboard_t *kb);
