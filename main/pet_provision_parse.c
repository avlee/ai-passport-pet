// main/pet_provision_parse.c
#include "pet_provision_parse.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// 一个极简的"单键 JSON 对象"解析器
// ---------------------------------------------------------------------------
// 协议里每条命令都是 {"键":值} 这一种形状: 没有嵌套、没有数组、一次一个键。
// 为此引一个通用 JSON 库并不划算(多几万字节 flash), 而且这里要处理的输入完全
// 来自我们自己的 Mac 客户端, 形状是受控的 —— 那就把它写死, 并且让主机测试能
// 直接覆盖到边界情况。
//
// 明确不支持的: 一条命令里多个键、\uXXXX 转义。客户端发的是原始 UTF-8, 中文
// SSID 不需要转义。

static void skip_blank(const char **p)
{
    while (**p == ' ' || **p == '\t') (*p)++;
}

// 读一个 JSON 字符串字面量(含转义)。放不下或格式不对都返回 false。
static bool read_string(const char **p, char *out, size_t size)
{
    if (**p != '"') return false;
    (*p)++;

    size_t n = 0;
    while (**p != '"') {
        char c = **p;
        if (c == '\0') return false;   // 收尾引号不见了
        if (c == '\\') {
            (*p)++;
            switch (**p) {
            case '"':  c = '"';  break;
            case '\\': c = '\\'; break;
            case '/':  c = '/';  break;
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            default: return false;
            }
        }
        if (n + 1 >= size) return false;   // 值太长, 由调用方按字段报错
        out[n++] = c;
        (*p)++;
    }
    out[n] = '\0';
    (*p)++;   // 收尾引号
    return true;
}

static bool read_int(const char **p, int *out)
{
    if (**p < '0' || **p > '9') return false;

    long value = 0;
    while (**p >= '0' && **p <= '9') {
        value = value * 10 + (**p - '0');
        if (value > 1000000) return false;
        (*p)++;
    }
    *out = (int)value;
    return true;
}

static bool read_bool(const char **p, bool *out)
{
    if (strncmp(*p, "true", 4) == 0) {
        *p += 4;
        *out = true;
        return true;
    }
    if (strncmp(*p, "false", 5) == 0) {
        *p += 5;
        *out = false;
        return true;
    }
    return false;
}

bool pet_prov_parse_line(const char *line, pet_prov_parse_t *out,
                         const char **error)
{
    // 静态字面量: 错误原因要活到调用方用完为止。
    static const char *const ERR_JSON = "bad json";
    static const char *const ERR_KEY = "unknown key";
    static const char *const ERR_TYPE = "bad value";
    static const char *const ERR_LONG = "too long";

    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    if (error) *error = ERR_JSON;
    if (line == NULL) return false;

    const char *p = line;
    skip_blank(&p);
    if (*p == '\0' || *p == '\r' || *p == '\n') {
        out->cmd = PET_PROV_CMD_NONE;   // 纯空行不是错误
        return true;
    }
    if (*p != '{') return false;
    p++;
    skip_blank(&p);
    if (*p != '"') return false;
    p++;

    char key[16];
    size_t key_len = 0;
    while (*p != '"') {
        if (*p == '\0' || key_len + 1 >= sizeof(key)) return false;
        key[key_len++] = *p++;
    }
    key[key_len] = '\0';
    p++;
    skip_blank(&p);
    if (*p != ':') return false;
    p++;
    skip_blank(&p);

    char value[PET_SETTINGS_PASS_MAX];
    value[0] = '\0';
    int number = 0;
    bool flag = false;

    const bool is_string = (*p == '"');
    if (is_string) {
        if (!read_string(&p, value, sizeof(value))) {
            if (error) *error = ERR_LONG;
            return false;
        }
    } else if (!read_int(&p, &number) && !read_bool(&p, &flag)) {
        return false;
    }

    // 值之后只允许空白和右花括号: 多字段命令本协议不支持, 明着拒绝比默默取一个
    // 键要好 —— 否则客户端以为整条都生效了。
    skip_blank(&p);
    if (*p != '}') {
        if (error) *error = ERR_JSON;
        return false;
    }

    if (strcmp(key, "pin") == 0) {
        if (!is_string) { if (error) *error = ERR_TYPE; return false; }
        out->cmd = PET_PROV_CMD_PIN;
    } else if (strcmp(key, "ssid") == 0) {
        // 空 SSID 没有意义, 直接挡掉, 免得设备拿着空名字反复重连。
        if (!is_string || value[0] == '\0') { if (error) *error = ERR_TYPE; return false; }
        if (strlen(value) >= PET_SETTINGS_SSID_MAX) { if (error) *error = ERR_LONG; return false; }
        out->cmd = PET_PROV_CMD_SSID;
    } else if (strcmp(key, "pass") == 0) {
        // 空密码是合法的(开放网络), 所以这里不检查长度以外的内容。
        if (!is_string) { if (error) *error = ERR_TYPE; return false; }
        out->cmd = PET_PROV_CMD_PASS;
    } else if (strcmp(key, "host") == 0) {
        if (!is_string || value[0] == '\0') { if (error) *error = ERR_TYPE; return false; }
        if (strlen(value) >= PET_SETTINGS_HOST_MAX) { if (error) *error = ERR_LONG; return false; }
        out->cmd = PET_PROV_CMD_HOST;
    } else if (strcmp(key, "port") == 0) {
        if (is_string || number < 1 || number > 65535) {
            if (error) *error = ERR_TYPE;
            return false;
        }
        out->cmd = PET_PROV_CMD_PORT;
        out->number = number;
    } else if (strcmp(key, "commit") == 0) {
        // 只认 true: {"commit":false} 多半是客户端状态没同步对, 静默忽略更容易
        // 让人以为是设备的问题。
        if (is_string || !flag) { if (error) *error = ERR_TYPE; return false; }
        out->cmd = PET_PROV_CMD_COMMIT;
        return true;
    } else if (strcmp(key, "forget") == 0) {
        if (is_string || !flag) { if (error) *error = ERR_TYPE; return false; }
        out->cmd = PET_PROV_CMD_FORGET;
        return true;
    } else {
        if (error) *error = ERR_KEY;
        return false;
    }

    snprintf(out->text, sizeof(out->text), "%s", value);
    return true;
}

bool pet_prov_pin_is_valid(const char *pin)
{
    if (pin == NULL) return false;
    for (int i = 0; i < 4; i++) {
        if (pin[i] < '0' || pin[i] > '9') return false;
    }
    return pin[4] == '\0';
}

// ---------------------------------------------------------------------------
// 状态事件拼装
// ---------------------------------------------------------------------------
typedef struct {
    char  *buf;
    size_t size;
    size_t used;
    bool   overflow;
} strbuf_t;

// 原样追加。JSON 的**结构**字符({ } " : , 和字符串外的字段名)走这里 ——
// 它们本来就要以裸引号的形式出现在报文里, 不参与转义。
static void sb_put(strbuf_t *sb, const char *text)
{
    for (const char *p = text; *p != '\0'; p++) {
        if (sb->used + 2 > sb->size) {   // 留出结尾 NUL
            sb->overflow = true;
            return;
        }
        sb->buf[sb->used++] = *p;
    }
    sb->buf[sb->used] = '\0';
}

// 转义后追加。**外部来的内容**(事件名、原因码、字段名、字段值、SSID、主机名)
// 走这里 —— 它们里面可能带引号或反斜杠, 直接拼进去会拼出非法 JSON。
//
// 这两个函数必须分开: 早先只有这一个函数、且它负责转义, 结果连结构用的引号也被
// 转义成了 \", 发出去的是 {\"event\":\"paired\"} —— 对端按 JSON 解必然失败, 而且
// 失败现象(界面一直停在"等待配对")看起来跟转义毫无关系。
static void sb_put_escaped(strbuf_t *sb, const char *text)
{
    for (const char *p = text; *p != '\0'; p++) {
        const char *escape = NULL;
        switch (*p) {
        case '"':  escape = "\\\""; break;
        case '\\': escape = "\\\\"; break;
        case '\n': escape = "\\n";  break;
        case '\r': escape = "\\r";  break;
        case '\t': escape = "\\t";  break;
        default: break;
        }

        const size_t need = (escape != NULL) ? 2u : 1u;
        if (sb->used + need + 1 > sb->size) {   // 留出结尾 NUL
            sb->overflow = true;
            return;
        }
        if (escape != NULL) {
            sb->buf[sb->used++] = escape[0];
            sb->buf[sb->used++] = escape[1];
        } else {
            sb->buf[sb->used++] = *p;
        }
    }
    sb->buf[sb->used] = '\0';
}

bool pet_prov_status_json(char *out, size_t size, const char *event,
                          const char *message, const char *field,
                          const char *value)
{
    if (out == NULL || size == 0 || event == NULL) return false;

    strbuf_t sb = { .buf = out, .size = size, .used = 0, .overflow = false };
    out[0] = '\0';

    sb_put(&sb, "{\"event\":\"");
    if (sb.overflow) return false;
    sb_put_escaped(&sb, event);
    sb_put(&sb, "\"");

    if (message != NULL && message[0] != '\0') {
        sb_put(&sb, ",\"message\":\"");
        sb_put_escaped(&sb, message);
        sb_put(&sb, "\"");
    }
    if (field != NULL && value != NULL) {
        sb_put(&sb, ",\"");
        sb_put_escaped(&sb, field);
        sb_put(&sb, "\":\"");
        sb_put_escaped(&sb, value);
        sb_put(&sb, "\"");
    }
    sb_put(&sb, "}");

    // 截断过的 JSON 绝对不能发出去, 对端会解析失败, 而失败原因看起来跟配网无关。
    return !sb.overflow;
}

bool pet_prov_escape_json(const char *src, char *dst, size_t size)
{
    if (src == NULL || dst == NULL || size == 0) return false;

    strbuf_t sb = { .buf = dst, .size = size, .used = 0, .overflow = false };
    dst[0] = '\0';
    sb_put_escaped(&sb, src);
    return !sb.overflow;
}
