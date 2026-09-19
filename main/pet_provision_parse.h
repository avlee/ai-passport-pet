// main/pet_provision_parse.h
// 蓝牙配网的命令解析与状态拼装。
//
// 刻意与 NimBLE 无关: 这一层是纯字符串逻辑, 所以主机测试能直接链接它
// (见 tests/test_pet_provision.c), 不必在 Mac 上搭一个假 BLE 栈。
//
// 线上格式与 TCP 那条链路一致 —— JSON 行。Mac 侧每次写一个完整的 JSON 对象,
// 设备侧按换行切分, 所以一条命令被拆成几次 ATT 写也能拼回来。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "pet_settings.h"

// 单条命令最长多少字节(不含结尾 NUL)。超过就丢弃并报错, 避免有人一直写不发换行
// 把接收缓冲撑爆。
#define PET_PROV_LINE_MAX 512

typedef enum {
    PET_PROV_CMD_NONE = 0,   // 空行, 忽略
    PET_PROV_CMD_PIN,        // {"pin":"1234"}
    PET_PROV_CMD_SSID,       // {"ssid":"home-2.4G"}
    PET_PROV_CMD_PASS,       // {"pass":"..."}
    PET_PROV_CMD_HOST,       // {"host":"192.168.0.10"}
    PET_PROV_CMD_PORT,       // {"port":8765}
    PET_PROV_CMD_COMMIT,     // {"commit":true}   保存并联网
    PET_PROV_CMD_FORGET,     // {"forget":true}   清掉存档, 回到未配网
    PET_PROV_CMD_REJECTED,   // 认得出是命令, 但不合法(见 error)
} pet_prov_cmd_t;

typedef struct {
    pet_prov_cmd_t cmd;
    char           text[PET_SETTINGS_PASS_MAX];  // pin/ssid/pass/host 的值
    int            number;                       // port
} pet_prov_parse_t;

// 解析一行命令。返回 false 表示这一行必须被拒绝, *error 给出简短原因
// (纯 ASCII, 由 Mac 侧翻译成中文提示)。
//
// 空行返回 true 且 cmd = PET_PROV_CMD_NONE。
bool pet_prov_parse_line(const char *line, pet_prov_parse_t *out,
                         const char **error);

// 配对码是否 4 位数字。
bool pet_prov_pin_is_valid(const char *pin);

// 把 src 里的 " 和 \ 转义后写进 dst(含结尾 NUL)。放不下返回 false。
// 用户输入的 SSID / 主机名可能带这些字符, 直接拼进 JSON 会拼出非法报文。
bool pet_prov_escape_json(const char *src, char *dst, size_t size);

// 拼一条状态事件:
//   {"event":"<event>"}
//   {"event":"<event>","message":"<message>"}
//   {"event":"<event>","<field>":"<value>"}
//   {"event":"<event>","message":"<message>","<field>":"<value>"}
// message / field 传 NULL 表示省略该字段。放不下返回 false。
bool pet_prov_status_json(char *out, size_t size, const char *event,
                          const char *message, const char *field,
                          const char *value);
