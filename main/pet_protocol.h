// main/pet_protocol.h
// Pet Bridge 的线路协议解析器。
//
// 传输是 TCP 上的 UTF-8 文本, 每条消息一行, 内容是扁平 JSON 对象:
//
// Mac -> 设备
//     {"type":"state","state":"working","text":"正在重构登录模块"}
//     {"type":"text","text":"只更新文本, 不改状态"}
//     {"type":"ping"}
//     {"type":"limits","primary":98,"weekly":31,"plan":"plus"}
//       Codex 用量限额: primary = 5 小时窗、weekly = 周窗的**已用**百分比(0..100)。
//       某个窗口没有可用快照时对应的字段整个不下发 —— 设备把缺的窗口藏起来。
//       plan 是订阅档位(plus/pro/…), 由桥接从同一条限额快照里取 plan_type 原样透传,
//       顶栏在电量下面显示成一个徽标。档位名是 OpenAI 的标识符, 设备侧不做翻译。
//     {"type":"pet","id":"sophie-portrait","size":1234567,"crc32":3735928559}
//       宣告"接下来 size 个字节就是这只宠物包"。这一行之后**不再是文本**,
//       而是紧跟 size 个裸字节; 收满之后链路自动回到行模式。
//     {"type":"sound","clip":"taskdone"}
//       让设备播放一段内嵌的提示音。clip 是固件里注册过的片段名; 固件不认识
//       的名字**静默忽略** —— 主机可以比固件新, 协议向前兼容。
//
//   设备 -> Mac  (由 pet_bridge.c 生成)
//     {"type":"hello","fw":"0.1.0","pet":"sophie-portrait"}
//       pet 为 "none" 表示槽是空的 —— Mac 侧看到这个就把默认那只推下来。
//     {"type":"battery","soc":95}      电量变化时上报; soc 为 null 表示读不到
//     {"type":"petdone","id":"...","ok":true|false}
//       一次宠物传输结束(成功或失败), 让菜单栏不必靠超时猜。
//     {"type":"poke"}                  用户戳了宠物一下
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
// 订阅档位最大字节数(含结尾 NUL)。桥接侧已把 plan 限长到 LIMITS_PLAN_MAX_LEN
// (24 个字符)并做过字符白名单, 这里只留余量兜底。
#define PET_PROTOCOL_PLAN_MAX 32
// 提示音片段名最大字节数(含结尾 NUL)。片段名是固件注册表里的短标识
// (如 "taskdone"), 桥接只透传字符串; 超长的按"没有这行"丢弃。
#define PET_PROTOCOL_CLIP_MAX 24

typedef enum {
    PET_MSG_NONE = 0,
    PET_MSG_STATE,   // 更新 Codex 状态, 可能同时带文本
    PET_MSG_TEXT,    // 只更新文本
    PET_MSG_PING,    // 心跳
    PET_MSG_LIMITS,  // Codex 用量限额快照(5 小时窗 / 周窗)
    PET_MSG_SOUND,   // 播放一段内嵌提示音, 片段名在 sound_clip 里
    // 宠物包传输的宣告。本行之后是**二进制载荷**, 不是文本 —— 解析器只负责把这一行
    // 解出来, 切分载荷是 pet_bridge.c 的事(它知道要按字节数收多久)。
    PET_MSG_PET,
} pet_msg_type_t;

typedef struct {
    pet_msg_type_t    type;
    bool              has_state;
    pet_codex_state_t state;
    bool              has_text;
    char              text[PET_PROTOCOL_TEXT_MAX];
    // 仅 PET_MSG_LIMITS: 各窗口的已用百分比, 0..100; -1 = 该窗口未提供。
    int8_t            limits_primary_used;
    int8_t            limits_weekly_used;
    // 仅 PET_MSG_LIMITS: 订阅档位(如 "plus")。has_limits_plan 为假时该字段无意义,
    // 界面据此把徽标整个藏起来 —— 与"缺哪个窗口藏哪个"同样的处理。
    bool              has_limits_plan;
    char              limits_plan[PET_PROTOCOL_PLAN_MAX];
    // 仅 PET_MSG_SOUND: 提示音片段名(如 "taskdone")。解析器不做白名单 —— 固件
    // 不认识的名字由播放器静默忽略, 主机可以比固件新。
    char              sound_clip[PET_PROTOCOL_CLIP_MAX];
    // 仅 PET_MSG_PET: 包的字节数与整包 CRC32, 以及 id(放在 text 里, 最长 31 字节)。
    uint32_t          pet_size;
    uint32_t          pet_crc32;
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

// 把线路上的订阅档位(如 "plus")整成界面上的展示名(如 "Plus")。
//
// 大小写规则与菜单栏那一侧**同一条**(见 tools/menubar/Sources/BridgeSupervisor.swift
// 的 codexPlanLabel): 去掉首尾空白, 首字母大写, 其余原样。桥接只做限长与字符白名单、
// 原样透传, 展示格式化落在各自显示面上 —— 设备这一份由 tests/test_pet_protocol.c 钉住,
// 预览工具(tools/preview_pet_screen.py)是同一规则的 Python 镜像。
//
// 另外丢掉非可打印 ASCII 字节: 徽标用的是内建 Montserrat 12, 字库只有拉丁字形, 画不
// 出来的字符只会渲染成缺字方框。桥接的白名单用的是 Python 的 isalnum(), 对非 ASCII
// 是放行的, 所以这条兜底不是假想。
//
// 返回 true 表示产出了非空展示名(写进 output); 返回 false 时调用方应把徽标整个藏起来。
bool pet_plan_label(const char *plan, char *output, size_t output_size);
