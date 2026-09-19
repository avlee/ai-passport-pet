// Host test for the Bluetooth provisioning command parser.
//
// 这一层是配网唯一"能在 Mac 上跑"的部分: 命令解析、配对码格式、状态 JSON 拼装
// 都是纯字符串逻辑。串口/NimBLE 那半边只能在真机上验, 但只要这里覆盖住了,
// "客户端发的字节被理解成了什么"就不会出意外。
//
// 重点覆盖的是**拒绝路径**: 配网报文来自网络那一侧, 宽松解析会把一个拼错的
// JSON 变成一次真的写配置 —— 那比直接报错难查得多。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pet_provision_parse.h"
#include "pet_settings.h"

// 解析一行并断言结果。error 传 NULL 表示不关心原因。
static bool parse(const char *line, pet_prov_cmd_t expect)
{
    pet_prov_parse_t parsed;
    const char *error = NULL;
    const bool ok = pet_prov_parse_line(line, &parsed, &error);
    if (!ok) return false;
    assert(parsed.cmd == expect);
    return true;
}

// 断言被拒, 并核对原因码。
static void reject(const char *line, const char *expect_error)
{
    pet_prov_parse_t parsed;
    const char *error = NULL;
    const bool ok = pet_prov_parse_line(line, &parsed, &error);
    assert(!ok);
    assert(error != NULL);
    if (expect_error != NULL) {
        if (strcmp(error, expect_error) != 0) {
            fprintf(stderr, "line %s: 期望原因 %s, 实际 %s\n", line, expect_error,
                    error);
            assert(0);
        }
    }
}

static void test_pin_command(void)
{
    pet_prov_parse_t parsed;
    const char *error = NULL;

    assert(pet_prov_parse_line("{\"pin\":\"1234\"}", &parsed, &error));
    assert(parsed.cmd == PET_PROV_CMD_PIN);
    assert(strcmp(parsed.text, "1234") == 0);

    // 解析层不管"是不是 4 位数字" —— 那是配对码校验的事。这里只保证字符串原样
    // 传下去, 好让 pet_prov_pin_is_valid 成为唯一的判断点。
    assert(pet_prov_parse_line("{\"pin\":\"12\"}", &parsed, &error));
    assert(parsed.cmd == PET_PROV_CMD_PIN);

    reject("{\"pin\":1234}", "bad value");      // 数字而不是字符串
    reject("{\"pin\":true}", "bad value");
}

static void test_pin_is_valid(void)
{
    assert(pet_prov_pin_is_valid("1234"));
    assert(pet_prov_pin_is_valid("0000"));
    assert(pet_prov_pin_is_valid("9999"));

    assert(!pet_prov_pin_is_valid("123"));      // 太短
    assert(!pet_prov_pin_is_valid("12345"));    // 太长
    assert(!pet_prov_pin_is_valid("12a4"));     // 非数字
    assert(!pet_prov_pin_is_valid("12 4"));     // 空格不算数字
    assert(!pet_prov_pin_is_valid("-123"));
    assert(!pet_prov_pin_is_valid(""));
    assert(!pet_prov_pin_is_valid(NULL));
    // 4 位数字 + 一个多余字符必须被拒: 否则 "1234\n" 这种会溜进去。
    assert(!pet_prov_pin_is_valid("1234\n"));
    // 全角数字要被拒(用户从小键盘以外的输入法敲进来过)。
    assert(!pet_prov_pin_is_valid("\xEF\xBC\x91\xEF\xBC\x92\xEF\xBC\x93\xEF\xBC\x94"));
}

static void test_ssid(void)
{
    pet_prov_parse_t parsed;
    const char *error = NULL;

    assert(pet_prov_parse_line("{\"ssid\":\"home-2.4G\"}", &parsed, &error));
    assert(parsed.cmd == PET_PROV_CMD_SSID);
    assert(strcmp(parsed.text, "home-2.4G") == 0);

    // 中文 SSID 按原始 UTF-8 传, 不要求转义。
    assert(pet_prov_parse_line("{\"ssid\":\"\xE5\xAE\xA2\xE5\x8E\x85-2.4G\"}", &parsed,
                               &error));
    assert(strcmp(parsed.text, "\xE5\xAE\xA2\xE5\x8E\x85-2.4G") == 0);

    // 长度边界: 字段上限是 33(含结尾 NUL), 所以 32 个字节能装下。
    char line[128];
    char ssid[PET_SETTINGS_SSID_MAX + 1];
    memset(ssid, 'a', PET_SETTINGS_SSID_MAX - 1);
    ssid[PET_SETTINGS_SSID_MAX - 1] = '\0';
    snprintf(line, sizeof(line), "{\"ssid\":\"%s\"}", ssid);
    assert(pet_prov_parse_line(line, &parsed, &error));
    assert(parsed.cmd == PET_PROV_CMD_SSID);
    assert(strlen(parsed.text) == PET_SETTINGS_SSID_MAX - 1);

    // 再长一个字符就得拒 —— 否则值会被悄悄截断, 用户看到"密码没错但连不上"。
    memset(ssid, 'a', PET_SETTINGS_SSID_MAX);
    ssid[PET_SETTINGS_SSID_MAX] = '\0';
    snprintf(line, sizeof(line), "{\"ssid\":\"%s\"}", ssid);
    reject(line, "too long");

    // 空 SSID 没有意义: 设备会拿着空名字反复重连。
    reject("{\"ssid\":\"\"}", "bad value");
    // null 连"值"都不算 —— 解析在认键之前就失败了, 所以原因归到 bad json。
    reject("{\"ssid\":null}", "bad json");
}

static void test_pass(void)
{
    pet_prov_parse_t parsed;
    const char *error = NULL;

    assert(pet_prov_parse_line("{\"pass\":\"hunter2!\"}", &parsed, &error));
    assert(parsed.cmd == PET_PROV_CMD_PASS);
    assert(strcmp(parsed.text, "hunter2!") == 0);

    // 开放网络: 空密码是合法的, 不能像 SSID 那样挡掉。
    assert(pet_prov_parse_line("{\"pass\":\"\"}", &parsed, &error));
    assert(parsed.cmd == PET_PROV_CMD_PASS);
    assert(parsed.text[0] == '\0');

    // 密码里最常见的就是各种符号, 转义必须还原成原字符。
    assert(pet_prov_parse_line("{\"pass\":\"a\\\"b\\\\c\"}", &parsed, &error));
    assert(strcmp(parsed.text, "a\"b\\c") == 0);

    // 长度边界: 上限 65, 所以 64 个字节能装下。
    char line[160];
    char pass[PET_SETTINGS_PASS_MAX + 1];
    memset(pass, 'x', PET_SETTINGS_PASS_MAX - 1);
    pass[PET_SETTINGS_PASS_MAX - 1] = '\0';
    snprintf(line, sizeof(line), "{\"pass\":\"%s\"}", pass);
    assert(pet_prov_parse_line(line, &parsed, &error));
    assert(strlen(parsed.text) == PET_SETTINGS_PASS_MAX - 1);

    memset(pass, 'x', PET_SETTINGS_PASS_MAX);
    pass[PET_SETTINGS_PASS_MAX] = '\0';
    snprintf(line, sizeof(line), "{\"pass\":\"%s\"}", pass);
    reject(line, "too long");

    reject("{\"pass\":12345}", "bad value");
}

static void test_host(void)
{
    pet_prov_parse_t parsed;
    const char *error = NULL;

    assert(pet_prov_parse_line("{\"host\":\"192.168.0.10\"}", &parsed, &error));
    assert(parsed.cmd == PET_PROV_CMD_HOST);
    assert(strcmp(parsed.text, "192.168.0.10") == 0);

    // 也允许主机名, 所以这里不校验"是不是 IP"。
    assert(pet_prov_parse_line("{\"host\":\"mac.local\"}", &parsed, &error));
    assert(parsed.cmd == PET_PROV_CMD_HOST);

    reject("{\"host\":\"\"}", "bad value");

    char line[128];
    char host[PET_SETTINGS_HOST_MAX + 1];
    memset(host, 'h', PET_SETTINGS_HOST_MAX);
    host[PET_SETTINGS_HOST_MAX] = '\0';
    snprintf(line, sizeof(line), "{\"host\":\"%s\"}", host);
    reject(line, "too long");
}

static void test_port(void)
{
    pet_prov_parse_t parsed;
    const char *error = NULL;

    assert(pet_prov_parse_line("{\"port\":8765}", &parsed, &error));
    assert(parsed.cmd == PET_PROV_CMD_PORT);
    assert(parsed.number == 8765);

    assert(pet_prov_parse_line("{\"port\":1}", &parsed, &error));
    assert(parsed.number == 1);
    assert(pet_prov_parse_line("{\"port\":65535}", &parsed, &error));
    assert(parsed.number == 65535);

    // 0 和 65536 都不是可用的端口。越界的值如果被接受, 后面 bind 才会失败,
    // 那时错误已经离用户输入很远了。
    reject("{\"port\":0}", "bad value");
    reject("{\"port\":65536}", "bad value");
    reject("{\"port\":\"8765\"}", "bad value");   // 字符串形式的端口
    reject("{\"port\":-1}", "bad json");
}

static void test_commit_and_forget(void)
{
    pet_prov_parse_t parsed;
    const char *error = NULL;

    assert(pet_prov_parse_line("{\"commit\":true}", &parsed, &error));
    assert(parsed.cmd == PET_PROV_CMD_COMMIT);

    assert(pet_prov_parse_line("{\"forget\":true}", &parsed, &error));
    assert(parsed.cmd == PET_PROV_CMD_FORGET);

    // 只认 true。{"commit":false} 多半是客户端状态没同步对, 静默当成"已提交"
    // 会让人以为设备连上了。
    reject("{\"commit\":false}", "bad value");
    reject("{\"forget\":false}", "bad value");
    reject("{\"commit\":\"true\"}", "bad value");
    reject("{\"forget\":1}", "bad value");
}

static void test_blank_lines(void)
{
    assert(parse("", PET_PROV_CMD_NONE));
    assert(parse("   ", PET_PROV_CMD_NONE));
    assert(parse("\t", PET_PROV_CMD_NONE));
    assert(parse("\r", PET_PROV_CMD_NONE));
    assert(parse("\n", PET_PROV_CMD_NONE));
    assert(parse("\r\n", PET_PROV_CMD_NONE));

    pet_prov_parse_t parsed;
    const char *error = NULL;
    assert(pet_prov_parse_line("  {\"pin\":\"1234\"}", &parsed, &error));
    assert(parsed.cmd == PET_PROV_CMD_PIN);   // 前面有空白也算
}

static void test_malformed_json(void)
{
    // 报文来自网络另一侧, 形状错了必须明着拒。
    reject("pin", "bad json");
    reject("{\"pin\":\"1234\"", "bad json");      // 少了右花括号
    reject("{}", "bad json");                     // 没有键
    reject("{\"pin\"}", "bad json");              // 没有冒号
    reject("{\"pin\":}", "bad json");             // 没有值
    reject("{\"pin\" \"1234\"}", "bad json");     // 少了冒号
    reject("{1234}", "bad json");

    // 一条命令只能有一个键: 多键命令必须被拒, 否则客户端会以为整条都生效了。
    reject("{\"ssid\":\"a\",\"pass\":\"b\"}", "bad json");
    reject("{\"pin\":\"1234\",\"commit\":true}", "bad json");

    // 不认识的键: 单独一个原因码, 好让 Mac 侧提示"固件版本偏旧"。
    reject("{\"foo\":\"bar\"}", "unknown key");
    reject("{\"SSID\":\"a\"}", "unknown key");    // 大小写敏感

    // 收尾引号不见了 —— 调用方只知道"这行不合法"就够了, 具体原因可以不精确。
    reject("{\"pin\":\"12", NULL);
}

static void test_null_arguments(void)
{
    pet_prov_parse_t parsed;
    assert(!pet_prov_parse_line(NULL, &parsed, NULL));
    assert(!pet_prov_parse_line("{\"pin\":\"1234\"}", NULL, NULL));
}

static void test_status_json(void)
{
    char buf[256];

    // 只有事件名。
    assert(pet_prov_status_json(buf, sizeof(buf), "paired", NULL, NULL, NULL));
    assert(strcmp(buf, "{\"event\":\"paired\"}") == 0);

    // 带原因码。
    assert(pet_prov_status_json(buf, sizeof(buf), "error", "bad json", NULL, NULL));
    assert(strcmp(buf, "{\"event\":\"error\",\"message\":\"bad json\"}") == 0);

    // 带一个附加字段。
    assert(pet_prov_status_json(buf, sizeof(buf), "done", NULL, "ip", "192.168.1.7"));
    assert(strcmp(buf, "{\"event\":\"done\",\"ip\":\"192.168.1.7\"}") == 0);

    // 原因码 + 字段一起。
    assert(pet_prov_status_json(buf, sizeof(buf), "pin_error", "wrong pin", "left", "2"));
    assert(strcmp(buf,
                  "{\"event\":\"pin_error\",\"message\":\"wrong pin\",\"left\":\"2\"}") == 0);

    // 空 message 视为没有: 客户端不该收到一个空的 message 字段。
    assert(pet_prov_status_json(buf, sizeof(buf), "paired", "", NULL, NULL));
    assert(strcmp(buf, "{\"event\":\"paired\"}") == 0);

    // 只有一半的键值对也要省略, 不能拼出 "field":null 这种。
    assert(pet_prov_status_json(buf, sizeof(buf), "paired", NULL, "left", NULL));
    assert(strcmp(buf, "{\"event\":\"paired\"}") == 0);
    assert(pet_prov_status_json(buf, sizeof(buf), "paired", NULL, NULL, "2"));
    assert(strcmp(buf, "{\"event\":\"paired\"}") == 0);

    // 转义: 状态文本里可能出现引号(比如设备名), 不转义就拼出非法 JSON。
    assert(pet_prov_status_json(buf, sizeof(buf), "error", "a\"b", NULL, NULL));
    assert(strcmp(buf, "{\"event\":\"error\",\"message\":\"a\\\"b\"}") == 0);
    assert(pet_prov_status_json(buf, sizeof(buf), "error", "a\nb", NULL, NULL));
    assert(strcmp(buf, "{\"event\":\"error\",\"message\":\"a\\nb\"}") == 0);

    // 放不下必须返回 false, 而不是发一条截断的 JSON 出去 —— 那样对端解析失败,
    // 报出来的错会跟配网毫无关系。
    char tiny[16];
    assert(!pet_prov_status_json(tiny, sizeof(tiny), "pin_error", "wrong pin", NULL, NULL));

    // 参数保护。
    assert(!pet_prov_status_json(NULL, sizeof(buf), "paired", NULL, NULL, NULL));
    assert(!pet_prov_status_json(buf, 0, "paired", NULL, NULL, NULL));
    assert(!pet_prov_status_json(buf, sizeof(buf), NULL, NULL, NULL, NULL));
}

static void test_escape_json(void)
{
    char buf[64];

    assert(pet_prov_escape_json("plain", buf, sizeof(buf)));
    assert(strcmp(buf, "plain") == 0);

    assert(pet_prov_escape_json("a\"b", buf, sizeof(buf)));
    assert(strcmp(buf, "a\\\"b") == 0);

    assert(pet_prov_escape_json("a\\b", buf, sizeof(buf)));
    assert(strcmp(buf, "a\\\\b") == 0);

    assert(pet_prov_escape_json("", buf, sizeof(buf)));
    assert(strcmp(buf, "") == 0);

    // 中文不需要转义, 原样输出。
    assert(pet_prov_escape_json("\xE5\xAE\xA2\xE5\x8E\x85", buf, sizeof(buf)));
    assert(strcmp(buf, "\xE5\xAE\xA2\xE5\x8E\x85") == 0);

    // 越界必须报 false。
    char tiny[4];
    assert(!pet_prov_escape_json("abcdef", tiny, sizeof(tiny)));
    assert(!pet_prov_escape_json(NULL, buf, sizeof(buf)));
    assert(!pet_prov_escape_json("plain", NULL, sizeof(buf)));
    assert(!pet_prov_escape_json("plain", buf, 0));
}

int main(void)
{
    test_pin_command();
    test_pin_is_valid();
    test_ssid();
    test_pass();
    test_host();
    test_port();
    test_commit_and_forget();
    test_blank_lines();
    test_malformed_json();
    test_null_arguments();
    test_status_json();
    test_escape_json();
    printf("test_pet_provision: PASS\n");
    return 0;
}
