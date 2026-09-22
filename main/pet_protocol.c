// main/pet_protocol.c
#include "pet_protocol.h"

#include <string.h>

// 只为 PET_PKG_HEADER_SIZE 一个常量: 收到"接下来是宠物包"时先拦掉明显荒唐的长度。
// 本文件与 pet_pkg 一样不依赖 ESP-IDF/LVGL, 引用它不会破坏主机可测性。
#include "pet_pkg.h"

// ---------------------------------------------------------------------------
// UTF-8 工具
// ---------------------------------------------------------------------------

// 返回一个 Unicode 码点编码成 UTF-8 需要的字节数。
static size_t utf8_encode(uint32_t codepoint, char *out)
{
    if (codepoint < 0x80) {
        out[0] = (char)codepoint;
        return 1;
    }
    if (codepoint < 0x800) {
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint < 0x10000) {
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (codepoint >> 18));
    out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    out[3] = (char)(0x80 | (codepoint & 0x3F));
    return 4;
}

bool pet_protocol_utf8_boundary(const char *text, size_t offset)
{
    if (text == NULL) return false;
    if (offset == 0) return true;
    return ((unsigned char)text[offset] & 0xC0) != 0x80;
}

// 从 UTF-8 首字节推断该字符占几个字节。非法的首字节按单字节处理,
// 这样解析器不会因为输入损坏而卡住。
static size_t utf8_sequence_length(unsigned char lead)
{
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

// ---------------------------------------------------------------------------
// 极简 JSON 取值
// ---------------------------------------------------------------------------

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// 在 line 中找到 "key" 之后紧跟冒号的位置。找不到返回 NULL。
// 这里只做字面量匹配, 因为协议是固定 schema 的扁平对象, 不需要完整解析器。
static const char *find_key(const char *line, const char *key)
{
    const size_t key_len = strlen(key);
    for (const char *p = line; (p = strchr(p, '"')) != NULL; p++) {
        if (strncmp(p + 1, key, key_len) != 0) continue;
        if (p[1 + key_len] != '"') continue;
        const char *q = p + 2 + key_len;
        while (is_space(*q)) q++;
        if (*q == ':') return q + 1;
    }
    return NULL;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 解析 4 位十六进制转义。失败返回 0 并把 *ok 置为 false。
static uint32_t parse_hex4(const char *p, bool *ok)
{
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        const int digit = hex_digit(p[i]);
        if (digit < 0) {
            *ok = false;
            return 0;
        }
        value = (value << 4) | (uint32_t)digit;
    }
    return value;
}

// 读取 line 中 key 对应的字符串值, 去转义后写入 out。
// 返回 true 表示找到了该键; *truncated 表示值因缓冲区不足被截断。
static bool json_get_string(const char *line, const char *key, char *out,
                            size_t out_size, bool *truncated)
{
    out[0] = '\0';
    const char *p = find_key(line, key);
    if (p == NULL) return false;
    while (is_space(*p)) p++;
    if (*p != '"') return false;
    p++;

    if (truncated) *truncated = false;
    size_t written = 0;

    // 每个分支负责消费自己对应的输入字节, 循环末尾不再统一 p++, 否则
    // \uXXXX 这类多字节消费会多跳一个字符。
    while (*p != '\0' && *p != '"') {
        // 输入本身就是 UTF-8, 所以普通字节必须原样搬运; 只有转义解析出的
        // 码点才需要重新编码, 否则会把 E6 AD A3 变成 C3 A6 C2 AD C2 A3。
        typedef enum { EMIT_RAW, EMIT_CODEPOINT, EMIT_SKIP } emit_mode_t;
        emit_mode_t mode = EMIT_RAW;
        uint32_t codepoint = 0;
        const char *raw = p;  // EMIT_RAW 时要写出的原始字节
        size_t raw_len = 1;

        if (*p == '\\') {
            p++;  // 消费反斜杠, p 指向被转义字符本身。
            mode = EMIT_CODEPOINT;
            switch (*p) {
            case 'n': codepoint = '\n'; p++; break;
            case 't': codepoint = '\t'; p++; break;
            case 'r': codepoint = '\r'; p++; break;
            case 'b': codepoint = '\b'; p++; break;
            case 'f': codepoint = '\f'; p++; break;
            case '"': codepoint = '"'; p++; break;
            case '\\': codepoint = '\\'; p++; break;
            case '/': codepoint = '/'; p++; break;
            case 'u': {
                bool ok = true;
                const uint32_t high = parse_hex4(p + 1, &ok);
                if (!ok) {
                    // 非法转义: 保留字面量 'u', 后面 4 个字符继续按原样解析。
                    codepoint = 'u';
                    p++;
                    break;
                }
                p += 5;  // 消费 'u' 与 4 位十六进制。
                if (high >= 0xD800 && high <= 0xDBFF && p[0] == '\\' &&
                    p[1] == 'u') {
                    bool low_ok = true;
                    const uint32_t low = parse_hex4(p + 2, &low_ok);
                    if (low_ok && low >= 0xDC00 && low <= 0xDFFF) {
                        codepoint = 0x10000 + ((high - 0xD800) << 10) +
                                    (low - 0xDC00);
                        p += 6;
                    } else {
                        // 高代理后面不是合法低代理: 替换字符兜底, 不产生非法 UTF-8。
                        codepoint = 0xFFFD;
                    }
                } else if (high >= 0xD800 && high <= 0xDFFF) {
                    codepoint = 0xFFFD;  // 孤立代理
                } else {
                    codepoint = high;
                }
                if (codepoint == 0) mode = EMIT_SKIP;  // \u0000: 消费但不写出
                break;
            }
            case '\0':
                // 反斜杠后直接结束: 整个值到此为止。
                out[written] = '\0';
                return true;
            default:
                // 未定义的转义一律原样保留被转义的字符(可能是多字节 UTF-8 的一部分)。
                raw = p;
                p++;
                mode = EMIT_RAW;
                break;
            }
        } else {
            // 普通字节: 按完整的 UTF-8 序列整体搬运, 这样缓冲区不足时的
            // 截断只会发生在字符边界上, 不会写出半个汉字。
            raw = p;
            raw_len = utf8_sequence_length((unsigned char)*p);
            size_t available = 0;
            while (available < raw_len && p[available] != '\0' &&
                   p[available] != '"') {
                available++;
            }
            raw_len = available;
            p += available;
        }

        if (mode == EMIT_SKIP) continue;

        char encoded[4];
        const char *emit = raw;
        size_t emit_len = raw_len;
        if (mode == EMIT_CODEPOINT) {
            emit_len = utf8_encode(codepoint, encoded);
            emit = encoded;
        }

        // 预留 1 字节给结尾 NUL; 放不下就整体截断而不是写半个字符。
        if (written + emit_len + 1 > out_size) {
            if (truncated) *truncated = true;
            break;
        }
        memcpy(out + written, emit, emit_len);
        written += emit_len;
    }

    out[written] = '\0';
    return true;
}

// 读取 line 中 key 对应的无符号整数值。找不到或不是合法数字返回 false。
// CRC32 会超过 INT32_MAX, 所以不能走有符号解析; 也刻意不接受负数、小数和指数 ——
// 协议里这三个字段都是十进制整数, 出现别的写法就说明对面不是我们的 Bridge。
static bool json_get_u32(const char *line, const char *key, uint32_t *out)
{
    const char *p = find_key(line, key);
    if (p == NULL) return false;
    while (is_space(*p)) p++;
    if (*p < '0' || *p > '9') return false;

    uint64_t value = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (uint64_t)(*p - '0');
        if (value > 0xFFFFFFFFull) return false;  // 溢出: 当成非法, 不要截断成别的数
        p++;
    }
    *out = (uint32_t)value;
    return true;
}

// 读取 line 中 key 对应的百分比整数(0..100)。找不到 key 返回 false; 找到了但
// 不是纯十进制整数、或超出 0..100 也返回 false —— 百分比只有这一种合法写法,
// "98.5"、"-5"、"1e2" 都说明对面不是我们的 Bridge, 宁可丢弃也不要猜。
static bool json_get_pct(const char *line, const char *key, int8_t *out)
{
    const char *p = find_key(line, key);
    if (p == NULL) return false;
    while (is_space(*p)) p++;
    if (*p < '0' || *p > '9') return false;

    int value = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        if (value > 100) return false;  // 已超上限还继续读只会是垃圾
        p++;
    }
    // 数字后面必须紧跟合法的 JSON 值终止符: 否则 "98.5" 会被截成 98、"1e2" 被
    // 截成 1 —— 截断出来的数看着合法, 其实谁也不是它。
    if (*p != '\0' && *p != ',' && *p != '}' && !is_space(*p)) return false;
    *out = (int8_t)value;
    return true;
}

// ---------------------------------------------------------------------------
// 行解析
// ---------------------------------------------------------------------------

// 解析一行; 返回 true 表示确实产生了一条消息(未知类型不算)。
static bool parse_line(const char *line, pet_msg_cb_t cb, void *user)
{
    pet_message_t msg;
    memset(&msg, 0, sizeof(msg));

    char type[24];
    if (!json_get_string(line, "type", type, sizeof(type), NULL)) return false;

    bool truncated = false;
    if (strcmp(type, "state") == 0) {
        msg.type = PET_MSG_STATE;
    } else if (strcmp(type, "text") == 0) {
        msg.type = PET_MSG_TEXT;
    } else if (strcmp(type, "ping") == 0) {
        msg.type = PET_MSG_PING;
    } else if (strcmp(type, "limits") == 0) {
        // 用量限额。三个字段全缺的行没有任何信息量, 直接丢弃; 只缺一部分也接受 ——
        // 桥接的语义就是"缺哪个藏哪个"。
        const bool has_primary = json_get_pct(line, "primary",
                                              &msg.limits_primary_used);
        const bool has_weekly = json_get_pct(line, "weekly",
                                             &msg.limits_weekly_used);
        // 档位单独算"这一行没白来": 换档(plus -> pro)时两个窗口可能恰好都没读到
        // 值, 而徽标仍然该跟着变。空串不算 —— "plan":"" 什么都没说, 与整个键不发是
        // 一回事(界面那边只会把徽标藏起来)。
        msg.has_limits_plan = json_get_string(line, "plan", msg.limits_plan,
                                              sizeof(msg.limits_plan), NULL) &&
                              msg.limits_plan[0] != '\0';
        if (!has_primary && !has_weekly && !msg.has_limits_plan) return false;
        if (!has_primary) msg.limits_primary_used = -1;
        if (!has_weekly) msg.limits_weekly_used = -1;
        msg.type = PET_MSG_LIMITS;
        cb(&msg, user);
        return true;
    } else if (strcmp(type, "pet") == 0) {
        // 宠物包宣告。id/size/crc32 缺一不可 —— 少一个就没法判断该收多久、收对了没,
        // 所以不完整的宣告直接丢弃(不回调), 后面的字节会继续被当成文本解析, 结果是
        // 一堆无法识别的行 —— 但那也远好过把二进制当成包写进 flash。
        if (!json_get_string(line, "id", msg.text, sizeof(msg.text), &truncated) ||
            !json_get_u32(line, "size", &msg.pet_size) ||
            !json_get_u32(line, "crc32", &msg.pet_crc32)) {
            return false;
        }
        // id 要能放下, 也要能放进包头的 pet_id[32] —— 否则回执里报出去的 id 和
        // 真正装上的那只对不上, 菜单栏会一直以为换错了。
        if (truncated || msg.text[0] == '\0' ||
            strlen(msg.text) >= PET_PKG_ID_MAX) {
            return false;
        }
        if (msg.pet_size < PET_PKG_HEADER_SIZE) return false;
        msg.type = PET_MSG_PET;
        msg.has_text = true;
        cb(&msg, user);
        return true;
    } else {
        return false;  // 未知类型直接忽略, 便于协议向前扩展。
    }

    if (msg.type == PET_MSG_STATE) {
        char state_name[24];
        if (json_get_string(line, "state", state_name, sizeof(state_name), NULL)) {
            if (pet_codex_state_parse(state_name, &msg.state)) {
                msg.has_state = true;
            }
        }
    }

    if (msg.type == PET_MSG_STATE || msg.type == PET_MSG_TEXT) {
        if (json_get_string(line, "text", msg.text, sizeof(msg.text), &truncated)) {
            msg.has_text = true;
        }
        msg.truncated = truncated;
    }

    cb(&msg, user);
    return true;
}

void pet_protocol_init(pet_protocol_t *protocol)
{
    if (protocol == NULL) return;
    memset(protocol, 0, sizeof(*protocol));
}

size_t pet_protocol_feed(pet_protocol_t *protocol, const char *data, size_t len,
                         pet_msg_cb_t cb, void *user)
{
    if (protocol == NULL || data == NULL || cb == NULL) return 0;

    size_t emitted = 0;
    for (size_t i = 0; i < len; i++) {
        const char c = data[i];
        if (c != '\n') {
            if (protocol->overflow) continue;
            if (protocol->len + 1 >= sizeof(protocol->line)) {
                // 超长行: 丢弃整个逻辑行, 等下一个换行重新同步。
                protocol->overflow = true;
                protocol->dropped++;
                continue;
            }
            protocol->line[protocol->len++] = c;
            continue;
        }

        if (protocol->overflow) {
            protocol->overflow = false;
            protocol->len = 0;
            continue;
        }

        protocol->line[protocol->len] = '\0';
        protocol->len = 0;
        // 允许空行与 \r\n。
        size_t end = strlen(protocol->line);
        while (end > 0 && is_space(protocol->line[end - 1])) {
            protocol->line[--end] = '\0';
        }
        if (end == 0) continue;

        if (parse_line(protocol->line, cb, user)) emitted++;
    }
    return emitted;
}

bool pet_protocol_escape(const char *input, char *output, size_t output_size)
{
    if (input == NULL || output == NULL || output_size == 0) return false;
    size_t written = 0;
    bool complete = true;

    for (const char *p = input; *p != '\0'; p++) {
        const char *replacement = NULL;
        char scratch[3];
        switch (*p) {
        case '"': replacement = "\\\""; break;
        case '\\': replacement = "\\\\"; break;
        case '\n': replacement = "\\n"; break;
        case '\r': replacement = "\\r"; break;
        case '\t': replacement = "\\t"; break;
        default:
            if ((unsigned char)*p < 0x20) {
                // 其余控制字符用 \u00XX。
                static const char hex[] = "0123456789abcdef";
                scratch[0] = '\\';
                scratch[1] = 'u';
                scratch[2] = '\0';
                if (written + 8 + 1 > output_size) {
                    complete = false;
                    break;
                }
                output[written++] = '\\';
                output[written++] = 'u';
                output[written++] = '0';
                output[written++] = '0';
                output[written++] = hex[((unsigned char)*p >> 4) & 0xF];
                output[written++] = hex[(unsigned char)*p & 0xF];
                continue;
            }
            scratch[0] = *p;
            scratch[1] = '\0';
            replacement = scratch;
            break;
        }
        if (replacement == NULL) {
            complete = false;
            break;
        }
        const size_t needed = strlen(replacement);
        if (written + needed + 1 > output_size) {
            complete = false;
            break;
        }
        memcpy(output + written, replacement, needed);
        written += needed;
    }

    output[written] = '\0';
    return complete;
}

bool pet_plan_label(const char *plan, char *output, size_t output_size)
{
    if (output == NULL || output_size == 0) return false;

    // 先挑出可打印 ASCII。桥接的白名单(Python 的 isalnum())对非 ASCII 是放行的,
    // 而徽标字体只有拉丁字形 —— 与其画一片缺字方框, 不如那些字节根本不进来。
    size_t written = 0;
    if (plan != NULL) {
        for (const char *p = plan; *p != '\0' && written + 1 < output_size; p++) {
            const unsigned char c = (unsigned char)*p;
            if (c < 0x20 || c > 0x7E) continue;
            output[written++] = (char)c;
        }
    }
    output[written] = '\0';

    // 去掉首尾空白: 桥接 strip 过一次, 这里兜住手工构造或测试直接喂进来的数据。
    size_t start = 0;
    while (start < written && is_space(output[start])) start++;
    size_t end = written;
    while (end > start && is_space(output[end - 1])) end--;
    if (end == start) {
        output[0] = '\0';
        return false;   // 全是空白: 与"没读到档位"同义, 让调用方藏起徽标
    }
    if (start > 0) memmove(output, output + start, end - start);
    output[end - start] = '\0';

    // 首字母大写, 其余原样。只认 ASCII 字母 —— 其余字符(数字、'-'、'_')原样留着,
    // 值本身是 OpenAI 的档位标识, 我们不做翻译也不做猜测。
    if (output[0] >= 'a' && output[0] <= 'z') {
        output[0] = (char)(output[0] - 'a' + 'A');
    }
    return true;
}
