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

int main(void)
{
    test_state_message();
    test_crlf_and_blank_lines();
    test_unknown_type_is_ignored();
    test_unknown_state_value_keeps_message();
    test_text_only_message();
    test_escapes_and_unicode();
    test_raw_utf8_passthrough();
    test_partial_lines_across_feeds();
    test_overlong_line_is_dropped_and_resyncs();
    test_long_text_truncates_on_character_boundary();
    test_escape_helper();
    test_utf8_boundary_helper();
    return 0;
}
