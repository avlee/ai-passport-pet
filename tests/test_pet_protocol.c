// Host test for the Pet Bridge line protocol parser.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pet_protocol.h"

#define CAPTURE_MAX 8

typedef struct {
    pet_message_t messages[CAPTURE_MAX];
    size_t count;
} capture_t;

static void capture_cb(const pet_message_t *msg, void *user)
{
    capture_t *capture = (capture_t *)user;
    if (capture->count >= CAPTURE_MAX) return;
    capture->messages[capture->count++] = *msg;
}

static size_t feed(capture_t *capture, pet_protocol_t *protocol, const char *text)
{
    capture->count = 0;
    return pet_protocol_feed(protocol, text, strlen(text), capture_cb, capture);
}

static void test_state_message(void)
{
    pet_protocol_t protocol;
    pet_protocol_init(&protocol);
    capture_t capture = {0};

    assert(feed(&capture, &protocol,
                "{\"type\":\"state\",\"state\":\"working\",\"text\":\"hi\"}\n") == 1);
    assert(capture.count == 1);
    assert(capture.messages[0].type == PET_MSG_STATE);
    assert(capture.messages[0].has_state);
    assert(capture.messages[0].state == PET_CODEX_WORKING);
    assert(capture.messages[0].has_text);
    assert(strcmp(capture.messages[0].text, "hi") == 0);
}

static void test_crlf_and_blank_lines(void)
{
    pet_protocol_t protocol;
    pet_protocol_init(&protocol);
    capture_t capture = {0};

    assert(feed(&capture, &protocol, "\r\n\n{\"type\":\"ping\"}\r\n") == 1);
    assert(capture.messages[0].type == PET_MSG_PING);
}

static void test_unknown_type_is_ignored(void)
{
    pet_protocol_t protocol;
    pet_protocol_init(&protocol);
    capture_t capture = {0};

    assert(feed(&capture, &protocol, "{\"type\":\"future\",\"x\":1}\n") == 0);
    assert(feed(&capture, &protocol, "not json at all\n") == 0);
    assert(feed(&capture, &protocol, "") == 0);
}

static void test_unknown_state_value_keeps_message(void)
{
    pet_protocol_t protocol;
    pet_protocol_init(&protocol);
    capture_t capture = {0};

    assert(feed(&capture, &protocol,
                "{\"type\":\"state\",\"state\":\"busy\",\"text\":\"t\"}\n") == 1);
    assert(capture.messages[0].type == PET_MSG_STATE);
    assert(!capture.messages[0].has_state);
    assert(strcmp(capture.messages[0].text, "t") == 0);
}

// 宠物包宣告。这一行后面跟的是**裸字节**, 所以解析器必须把 size / crc32 / id 三条
// 都解出来; 少一条就没法判断该收多久、收对了没。
static void test_pet_announce_message(void)
{
    pet_protocol_t protocol;
    pet_protocol_init(&protocol);
    capture_t capture = {0};

    assert(feed(&capture, &protocol,
                "{\"type\":\"pet\",\"id\":\"sophie-portrait\",\"size\":1234567,"
                "\"crc32\":3735928559}\n") == 1);
    assert(capture.count == 1);
    assert(capture.messages[0].type == PET_MSG_PET);
    assert(strcmp(capture.messages[0].text, "sophie-portrait") == 0);
    assert(capture.messages[0].pet_size == 1234567);
    // 0xDEADBEEF: 大于 INT32_MAX, 走有符号解析就会变成负数。
    assert(capture.messages[0].pet_crc32 == 0xDEADBEEFu);
    assert(!capture.messages[0].truncated);
}

// 缺字段 / 长度荒唐的宣告一律丢弃, 而且**不回调** —— 一旦接受了半条宣告, 后面
// 几 MB 的二进制就会被当成文本喂给行解析器, 整个流再也对不上。
static void test_pet_announce_rejects_incomplete(void)
{
    pet_protocol_t protocol;
    pet_protocol_init(&protocol);
    capture_t capture = {0};

    assert(feed(&capture, &protocol,
                "{\"type\":\"pet\",\"id\":\"x\",\"size\":100}\n") == 0);  // 少 crc32
    assert(feed(&capture, &protocol,
                "{\"type\":\"pet\",\"id\":\"x\",\"crc32\":1}\n") == 0);    // 少 size
    assert(feed(&capture, &protocol,
                "{\"type\":\"pet\",\"size\":100,\"crc32\":1}\n") == 0);    // 少 id
    // 比头部还短的"包"没有意义。
    assert(feed(&capture, &protocol,
                "{\"type\":\"pet\",\"id\":\"x\",\"size\":8,\"crc32\":1}\n") == 0);
    // 超出 uint32 的长度是非法输入, 不能截断成另一个数。
    assert(feed(&capture, &protocol,
                "{\"type\":\"pet\",\"id\":\"x\",\"size\":4294967296,"
                "\"crc32\":1}\n") == 0);
    // id 太长(包头里只有 32 字节) —— 接受了的话回执报出去的 id 就和真正装上的
    // 那只对不上。
    assert(feed(&capture, &protocol,
                "{\"type\":\"pet\",\"id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
                "\"size\":100000,\"crc32\":1}\n") == 0);
    assert(capture.count == 0);
}

// 用量限额。两个百分比都是"已用", 0..100; 缺哪个字段哪个就是 -1, 两个都缺则
// 整行丢弃 —— 没有信息量的行不该惊动应用层。
static void test_limits_message(void)
{
    pet_protocol_t protocol;
    pet_protocol_init(&protocol);
    capture_t capture = {0};

    assert(feed(&capture, &protocol,
                "{\"type\":\"limits\",\"primary\":98,\"weekly\":31}\n") == 1);
    assert(capture.count == 1);
    assert(capture.messages[0].type == PET_MSG_LIMITS);
    assert(capture.messages[0].limits_primary_used == 98);
    assert(capture.messages[0].limits_weekly_used == 31);
    // 没带 plan 的旧行照收 —— 徽标那一侧据此把整块藏起来。
    assert(!capture.messages[0].has_limits_plan);

    // 只有一个窗口: 缺的那个是 -1。
    assert(feed(&capture, &protocol, "{\"type\":\"limits\",\"primary\":100}\n") == 1);
    assert(capture.messages[0].limits_primary_used == 100);
    assert(capture.messages[0].limits_weekly_used == -1);

    assert(feed(&capture, &protocol, "{\"type\":\"limits\",\"weekly\":0}\n") == 1);
    assert(capture.messages[0].limits_primary_used == -1);
    assert(capture.messages[0].limits_weekly_used == 0);

    // 两个都缺 / 值非法(小数、负数、超上限): 一律丢弃, 不回调。
    assert(feed(&capture, &protocol, "{\"type\":\"limits\"}\n") == 0);
    // 单个窗口非法但另一个合法: 非法的按"未提供"处理(-1), 整行仍接受 —— 与
    // "缺哪个藏哪个"是同一条语义, 只是触发原因从"没发"变成了"发得不对"。
    assert(feed(&capture, &protocol,
                "{\"type\":\"limits\",\"primary\":98.5,\"weekly\":31}\n") == 1);
    assert(capture.messages[0].limits_primary_used == -1);
    assert(capture.messages[0].limits_weekly_used == 31);
    assert(feed(&capture, &protocol,
                "{\"type\":\"limits\",\"primary\":-5,\"weekly\":31}\n") == 1);
    assert(capture.messages[0].limits_primary_used == -1);
    assert(feed(&capture, &protocol,
                "{\"type\":\"limits\",\"primary\":150,\"weekly\":31}\n") == 1);
    assert(capture.messages[0].limits_primary_used == -1);
    assert(feed(&capture, &protocol,
                "{\"type\":\"limits\",\"primary\":1e2,\"weekly\":31}\n") == 1);
    assert(capture.messages[0].limits_primary_used == -1);
    // 档位(plan): 桥接原样透传(不做中文映射也不做首字母大写), 解析器原样存下 ——
    // 展示格式化落在各自显示面上(见 test_plan_label_display_name)。
    assert(feed(&capture, &protocol,
                "{\"type\":\"limits\",\"primary\":98,\"weekly\":31,"
                "\"plan\":\"plus\"}\n") == 1);
    assert(capture.messages[0].has_limits_plan);
    assert(strcmp(capture.messages[0].limits_plan, "plus") == 0);

    // 只带 plan: 换档(plus -> pro)而两个窗口恰好都没读到值, 这一行仍然有信息量,
    // 不能让"两个窗口都缺"把换档一起丢掉。
    assert(feed(&capture, &protocol, "{\"type\":\"limits\",\"plan\":\"pro\"}\n") == 1);
    assert(capture.messages[0].has_limits_plan);
    assert(strcmp(capture.messages[0].limits_plan, "pro") == 0);
    assert(capture.messages[0].limits_primary_used == -1);
    assert(capture.messages[0].limits_weekly_used == -1);

    // 空档位值等于什么都没说: 既不算"有档位", 也撑不起一整行。
    assert(feed(&capture, &protocol, "{\"type\":\"limits\",\"plan\":\"\"}\n") == 0);

    // 两个窗口都非法: 等于什么都没说, 丢弃。
    assert(feed(&capture, &protocol,
                "{\"type\":\"limits\",\"primary\":1e2,\"weekly\":x}\n") == 0);
    assert(capture.count == 0);
}

static void test_text_only_message(void)
{
    pet_protocol_t protocol;
    pet_protocol_init(&protocol);
    capture_t capture = {0};

    assert(feed(&capture, &protocol, "{\"type\":\"text\",\"text\":\"only\"}\n") == 1);
    assert(capture.messages[0].type == PET_MSG_TEXT);
    assert(!capture.messages[0].has_state);
    assert(strcmp(capture.messages[0].text, "only") == 0);
}

static void test_escapes_and_unicode(void)
{
    pet_protocol_t protocol;
    pet_protocol_init(&protocol);
    capture_t capture = {0};

    // 引号 / 反斜杠 / 换行 转义。
    assert(feed(&capture, &protocol,
                "{\"type\":\"text\",\"text\":\"a\\\"b\\\\c\\nd\"}\n") == 1);
    assert(strcmp(capture.messages[0].text, "a\"b\\c\nd") == 0);

    // \u 转义解码为 UTF-8: U+4E2D U+6587 => 中文
    assert(feed(&capture, &protocol,
                "{\"type\":\"text\",\"text\":\"\\u4e2d\\u6587\"}\n") == 1);
    assert(strcmp(capture.messages[0].text, "\xE4\xB8\xAD\xE6\x96\x87") == 0);

    // 代理对: U+1F600 崩溃脸 => F0 9F 98 80
    assert(feed(&capture, &protocol,
                "{\"type\":\"text\",\"text\":\"\\ud83d\\ude00\"}\n") == 1);
    assert(strcmp(capture.messages[0].text, "\xF0\x9F\x98\x80") == 0);

    // 孤立高代理按替换字符处理, 不产生非法 UTF-8。
    assert(feed(&capture, &protocol,
                "{\"type\":\"text\",\"text\":\"\\ud83d\"}\n") == 1);
    assert(strcmp(capture.messages[0].text, "\xEF\xBF\xBD") == 0);

    // 非法 \u 转义原样保留, 不吞字符。
    assert(feed(&capture, &protocol,
                "{\"type\":\"text\",\"text\":\"\\uzzzz!\"}\n") == 1);
    assert(strcmp(capture.messages[0].text, "uzzzz!") == 0);
}

static void test_raw_utf8_passthrough(void)
{
    pet_protocol_t protocol;
    pet_protocol_init(&protocol);
    capture_t capture = {0};

    assert(feed(&capture, &protocol,
                "{\"type\":\"state\",\"state\":\"done\","
                "\"text\":\"正在重构登录模块\"}\n") == 1);
    assert(strcmp(capture.messages[0].text, "正在重构登录模块") == 0);
}

static void test_partial_lines_across_feeds(void)
{
    pet_protocol_t protocol;
    pet_protocol_init(&protocol);
    capture_t capture = {0};

    assert(feed(&capture, &protocol, "{\"type\":\"sta") == 0);
    assert(feed(&capture, &protocol, "te\",\"state\":\"re") == 0);
    assert(feed(&capture, &protocol, "ady\"}") == 0);
    assert(feed(&capture, &protocol, "\n") == 1);
    assert(capture.messages[0].state == PET_CODEX_READY);

    // 一条缓冲区里同时到达多条消息。
    assert(feed(&capture, &protocol,
                "{\"type\":\"ping\"}\n{\"type\":\"ping\"}\n") == 2);
}

static void test_overlong_line_is_dropped_and_resyncs(void)
{
    pet_protocol_t protocol;
    pet_protocol_init(&protocol);
    capture_t capture = {0};

    char big[PET_PROTOCOL_LINE_MAX * 2];
    memset(big, 'x', sizeof(big) - 2);
    big[sizeof(big) - 2] = '\n';
    big[sizeof(big) - 1] = '\0';

    assert(feed(&capture, &protocol, big) == 0);
    assert(protocol.dropped == 1);

    // 溢出行之后必须能正常同步到下一条合法消息。
    assert(feed(&capture, &protocol, "{\"type\":\"ping\"}\n") == 1);
    assert(capture.messages[0].type == PET_MSG_PING);
}

static void test_long_text_truncates_on_character_boundary(void)
{
    pet_protocol_t protocol;
    pet_protocol_init(&protocol);
    capture_t capture = {0};

    // 100 个三字节汉字 = 300 字节文本(整行仍在行长度上限之内),
    // 必须截断到 PET_PROTOCOL_TEXT_MAX 以内。
    char line[1024];
    int offset = snprintf(line, sizeof(line), "{\"type\":\"text\",\"text\":\"");
    for (int i = 0; i < 100; i++) {
        line[offset++] = (char)0xE4;
        line[offset++] = (char)0xB8;
        line[offset++] = (char)0xAD;
    }
    line[offset++] = '"';
    line[offset++] = '}';
    line[offset++] = '\n';
    line[offset] = '\0';

    assert(feed(&capture, &protocol, line) == 1);
    assert(capture.messages[0].truncated);

    const char *text = capture.messages[0].text;
    const size_t length = strlen(text);
    assert(length > 0);
    assert(length < PET_PROTOCOL_TEXT_MAX);
    // 每个汉字 3 字节: 长度必须是 3 的倍数, 且整串都是完整的 U+4E2D。
    assert(length % 3 == 0);
    for (size_t i = 0; i + 2 < length; i += 3) {
        assert(memcmp(text + i, "\xE4\xB8\xAD", 3) == 0);
    }
}

static void test_escape_helper(void)
{
    char out[64];
    assert(pet_protocol_escape("a\"b\\c\nd", out, sizeof(out)));
    assert(strcmp(out, "a\\\"b\\\\c\\nd") == 0);

    // UTF-8 原样保留。
    assert(pet_protocol_escape("中文", out, sizeof(out)));
    assert(strcmp(out, "中文") == 0);

    // 控制字符转成 \u00XX。
    assert(pet_protocol_escape("\x01", out, sizeof(out)));
    assert(strcmp(out, "\\u0001") == 0);

    // 缓冲区不足时返回 false 且仍然 NUL 结尾。
    char small[4];
    assert(!pet_protocol_escape("aaaaaaaa", small, sizeof(small)));
    assert(small[sizeof(small) - 1] == '\0');
}

static void test_utf8_boundary_helper(void)
{
    const char *text = "\xE4\xB8\xAD" "a";
    assert(pet_protocol_utf8_boundary(text, 0));
    assert(pet_protocol_utf8_boundary(text, 3));
    assert(!pet_protocol_utf8_boundary(text, 1));
    assert(!pet_protocol_utf8_boundary(text, 2));
    assert(!pet_protocol_utf8_boundary(NULL, 0));
}

// 订阅档位的展示名: 首字母大写, 其余原样, 非可打印 ASCII 丢掉。大小写规则与菜单栏
// 那一侧同一条(tools/menubar/Sources/BridgeSupervisor.swift 的 codexPlanLabel), 预览
// 工具里是同一规则的 Python 镜像 —— 三处必须表现一致, 否则同一个档位在屏幕和菜单里
// 会长得不一样。
static void test_plan_label_display_name(void)
{
    char out[PET_PROTOCOL_PLAN_MAX];

    // 线路上的原样值是小写的, 界面要的是首字母大写。
    assert(pet_plan_label("plus", out, sizeof(out)));
    assert(strcmp(out, "Plus") == 0);
    assert(pet_plan_label("pro", out, sizeof(out)));
    assert(strcmp(out, "Pro") == 0);

    // 已经是大写的原样返回(幂等)。
    assert(pet_plan_label("Plus", out, sizeof(out)));
    assert(strcmp(out, "Plus") == 0);

    // 首尾空白去掉: 桥接 strip 过一次, 这里兜住手工构造或测试直接喂进来的数据。
    assert(pet_plan_label("  team  ", out, sizeof(out)));
    assert(strcmp(out, "Team") == 0);

    // 非字母字符原样留着 —— 档位标识不是只有字母一种形状。
    assert(pet_plan_label("business-2", out, sizeof(out)));
    assert(strcmp(out, "Business-2") == 0);

    // 非 ASCII 直接丢: 徽标用的是内建 Montserrat 12, 只有拉丁字形, 画不出来只会
    // 渲染成缺字方框。桥接的白名单是 Python 的 isalnum(), 对非 ASCII 放行, 所以
    // 这条兜底不是假想。
    assert(pet_plan_label("plus\xE2\x9C\x93", out, sizeof(out)));  // plus✓
    assert(strcmp(out, "Plus") == 0);
    assert(!pet_plan_label("\xE4\xB8\xAD\xE6\x96\x87", out, sizeof(out)));  // 中文
    assert(out[0] == '\0');

    // 空 / 全空白 / NULL: 都表示"没有可显示的档位", 调用方据此整块藏徽标。
    assert(!pet_plan_label("", out, sizeof(out)));
    assert(!pet_plan_label(" \t ", out, sizeof(out)));
    assert(!pet_plan_label(NULL, out, sizeof(out)));
    assert(out[0] == '\0');

    // 缓冲区不够: 截断, 但仍然 NUL 结尾(徽标那边还会按可用宽度再省略一次)。
    char tight[4];
    assert(pet_plan_label("enterprise", tight, sizeof(tight)));
    assert(strcmp(tight, "Ent") == 0);

    // 非法参数返回 false, 不越界写。
    assert(!pet_plan_label("plus", NULL, sizeof(out)));
    assert(!pet_plan_label("plus", out, 0));
}

int main(void)
{
    test_state_message();
    test_crlf_and_blank_lines();
    test_unknown_type_is_ignored();
    test_unknown_state_value_keeps_message();
    test_pet_announce_message();
    test_pet_announce_rejects_incomplete();
    test_limits_message();
    test_text_only_message();
    test_escapes_and_unicode();
    test_raw_utf8_passthrough();
    test_partial_lines_across_feeds();
    test_overlong_line_is_dropped_and_resyncs();
    test_long_text_truncates_on_character_boundary();
    test_escape_helper();
    test_utf8_boundary_helper();
    test_plan_label_display_name();
    return 0;
}
