// tests/parse_pet_line.c
// 把 stdin 上的字节喂给设备侧的协议解析器, 每解析出一条消息打一行:
//     <type> <has_state> <state> <has_text> <text>
//     limits <primary> <weekly> <plan|->     (限额消息用这一种行式)
//
// 用途: tests/test_pet_bridge.py 把 Mac 侧 Pet Bridge 真正发出去的字节灌进来,
// 用设备自己的解析器读一遍再比对。这样协议的两端是互相校验的, 不靠人工对文档。
#include <stdio.h>
#include <string.h>

#include "pet_protocol.h"

static void report(const pet_message_t *msg, void *user)
{
    (void)user;
    if (msg->type == PET_MSG_LIMITS) {
        // 限额字段单独一行打出来, 供桥接侧测试逐字段比对。
        // plan 放在**最后**、调用方按 split(" ", 3) 切: 档位名理论上可能含空格
        // (桥接侧的白名单允许), 摆在后面就不会被切碎。
        printf("limits %d %d %s\n", (int)msg->limits_primary_used,
               (int)msg->limits_weekly_used,
               msg->has_limits_plan ? msg->limits_plan : "-");
        fflush(stdout);
        return;
    }
    const char *type = "none";
    switch (msg->type) {
    case PET_MSG_STATE: type = "state"; break;
    case PET_MSG_TEXT:  type = "text";  break;
    case PET_MSG_PING:  type = "ping";  break;
    case PET_MSG_NONE:
    default:            break;
    }
    printf("%s %d %s %d %s\n", type, msg->has_state ? 1 : 0,
           msg->has_state ? pet_codex_state_name(msg->state) : "-",
           msg->has_text ? 1 : 0, msg->has_text ? msg->text : "-");
    fflush(stdout);
}

int main(void)
{
    pet_protocol_t protocol;
    pet_protocol_init(&protocol);

    char buffer[256];
    size_t read_bytes;
    while ((read_bytes = fread(buffer, 1, sizeof(buffer), stdin)) > 0) {
        pet_protocol_feed(&protocol, buffer, read_bytes, report, NULL);
    }
    return 0;
}
