// main/pet_protocol.h
// Pet Bridge 的线路协议解析器。
//
// 传输是 TCP 上的 UTF-8 文本, 每条消息一行, 内容是扁平 JSON 对象:
//
//   Mac -> 设备
//     {"type":"state","state":"working","text":"正在重构登录模块"}
//     {"type":"text","text":"只更新文本, 不改状态"}
//     {"type":"ping"}
//
//   设备 -> Mac  (由 pet_bridge.c 生成)
//     {"type":"hello","fw":"0.1.0","pet":"sophie-portrait","state":"idle"}
//     {"type":"pong"}
//
// 解析必须是「有界且宽容」的: 单行长度、字段长度都有上限, 未知字段直接忽略,
// UTF-8 只在字符边界上截断。因此本文件不依赖 ESP-IDF/LVGL, 可在主机上做单元测试。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pet_state.h"

// 单行最大字节数(含结尾 NUL)。超长行会被丢弃并在下一个换行处重新同步。
#define PET_PROTOCOL_LINE_MAX 512
// 文本字段最大字节数(含结尾 NUL)。
#define PET_PROTOCOL_TEXT_MAX 192

typedef enum {
    PET_MSG_NONE = 0,
    PET_MSG_STATE,   // 更新 Codex 状态, 可能同时带文本
    PET_MSG_TEXT,    // 只更新文本
    PET_MSG_PING,    // 心跳
} pet_msg_type_t;

typedef struct {
    pet_msg_type_t    type;
    bool              has_state;
    pet_codex_state_t state;
    bool              has_text;
    char              text[PET_PROTOCOL_TEXT_MAX];
    // 非 0 表示该行虽然被接受, 但有内容被截断(字段过长)。
    bool              truncated;
} pet_message_t;

typedef void (*pet_msg_cb_t)(const pet_message_t *msg, void *user);

typedef struct {
    char   line[PET_PROTOCOL_LINE_MAX];
    size_t len;
    bool   overflow;      // 当前行已超长, 丢到下一个换行为止
    uint32_t dropped;     // 因超长被丢弃的行数, 供日志观测
} pet_protocol_t;

void pet_protocol_init(pet_protocol_t *protocol);

// 喂入一段接收到的字节。每解析出一条完整消息就回调一次 cb。
// 返回本次解析出的消息条数。
size_t pet_protocol_feed(pet_protocol_t *protocol, const char *data, size_t len,
                         pet_msg_cb_t cb, void *user);

// 把字符串转义成可以安全放进 JSON 字符串字面量的形式(不含首尾引号)。
// 无法放入时截断, 返回 false。
bool pet_protocol_escape(const char *input, char *output, size_t output_size);

// 判断字节是否为 UTF-8 序列的起始字节(用于外部截断前自检)。
bool pet_protocol_utf8_boundary(const char *text, size_t offset);
