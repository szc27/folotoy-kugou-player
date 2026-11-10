// main/kugou_proto.h —— 扫码登录协议中与平台无关的部分。
//
// 单独拆出来是为了在 PC 上做主机测试：URL 拼接与 JSON 字段提取写错，
// 编译和构建都不会报错，只会在真机上表现为「扫了码没反应」。
#pragma once

#include <stddef.h>

#include "kugou_model.h"

#define KG_QR_KEY_CAP  64
#define KG_QR_URL_CAP 256

// 把模板里的 %s 替换为二维码 key，生成指向外部 H5 登录页的完整 URL。
// 不做百分号编码之外的格式化，模板来自编译期配置，不接受运行时格式串。
// 返回写入长度，失败返回 -1。
int kg_proto_build_login_url(char *out, size_t cap,
                             const char *templ, const char *key);

// 从一段紧凑 JSON 中取字符串字段（只支持一层，够用于扫码接口响应）。
// 返回 0 表示成功；字段不存在、类型不对或缓冲区不足返回 -1。
int kg_proto_extract_string(const char *json, const char *field,
                            char *out, size_t cap);

// 从一段紧凑 JSON 中取整数字段。返回 0 表示成功。
int kg_proto_extract_int(const char *json, const char *field, int *out);

// 把扫码状态接口的数值状态映射为应用登录状态。
// 兼容两套常见编码：0/1/2/4 与 800/801/802/803。
kg_login_t kg_proto_login_state(int status);
