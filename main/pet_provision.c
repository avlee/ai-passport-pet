// main/pet_provision.c
#include "pet_provision.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "pet_config.h"
#include "pet_provision_parse.h"
// 配网信息里的 pet 字段报的是槽里当前那只, 不再是编译期常量。
#include "pet_slot.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "pet_provision";

// ---------------------------------------------------------------------------
// 服务与特征
// ---------------------------------------------------------------------------
// 规范形式(大端)的 128 位 UUID, 四个只有倒数第 4 个字节不同。设备侧用
// ble_uuid_from_str() 解析, Mac 侧用同一个字符串构造 CBUUID —— 字节序交给库去
// 处理, 免得手写 BLE_UUID128_INIT 的小端字节数组时写反了只能靠真机才发现。
//
//   c0de0001  service   配网服务
//   c0de0002  cmd       写入命令(JSON 行)
//   c0de0003  status    状态通知 + 可读(回报最后一次状态)
//   c0de0004  info      只读设备信息
#define PROV_UUID_SERVICE "c0de0001-7e57-4a7c-9d20-706574627269"
#define PROV_UUID_CMD     "c0de0002-7e57-4a7c-9d20-706574627269"
#define PROV_UUID_STATUS  "c0de0003-7e57-4a7c-9d20-706574627269"
#define PROV_UUID_INFO    "c0de0004-7e57-4a7c-9d20-706574627269"

// 单次 ATT 写的上限。协商出来的 MTU 一般是 256(见 sdkconfig), 即 253 字节净荷;
// 留一点余量, 超过就丢弃而不是越界。
#define PROV_CHUNK_MAX 256

// 收齐参数、成功联网之后, 窗口再留一会儿才自动关闭, 好让 Mac 侧收到 done。
#define PROV_CLOSE_AFTER_DONE_MS 3000

// 配置里的 PET_PROVISION_APPLY_TIMEOUT_MS 由应用层的 apply 回调使用
// (它负责等 IP); 这里只做编译期提醒, 避免有人只改了一边。
_Static_assert(PET_PROVISION_APPLY_TIMEOUT_MS > 0, "配网等待 IP 的时间必须为正");

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
static pet_settings_t s_draft;    // Mac 正在填的草稿
static pet_settings_t s_current;  // 当前生效的参数(供 info 回读)
static pet_provision_sink_t s_sink;
static volatile bool s_sink_valid;

static pet_provision_state_t s_state;
static char s_detail[64];         // 状态的简短说明, 给界面看
static char s_pin[PET_SETTINGS_PIN_MAX];
static int s_attempts_left;

static atomic_bool s_connected = ATOMIC_VAR_INIT(false);
static atomic_bool s_paired = ATOMIC_VAR_INIT(false);
static atomic_int s_conn_handle = ATOMIC_VAR_INIT(BLE_HS_CONN_HANDLE_NONE);
static volatile bool s_running;      // 配网窗口是否开着

static SemaphoreHandle_t s_state_lock;
static SemaphoreHandle_t s_tx_lock;

// 接收缓冲只在 NimBLE 宿主任务(GATT 回调)里读写, 所以不需要锁。
static char s_rx[PET_PROV_LINE_MAX];
static size_t s_rx_len;

// 状态上报: 由任意任务准备, 真正发送在宿主任务的 callout 里。
static struct ble_npl_callout s_notify_callout;
static char s_pending_status[PET_PROV_LINE_MAX];
static char s_last_status[PET_PROV_LINE_MAX];
static volatile bool s_ble_ready;   // NimBLE 是否还活着(决定能否安排 callout)

static QueueHandle_t s_apply_queue;  // 草稿快照 -> 配网任务
static TaskHandle_t s_task;

static bool s_initialized;           // NimBLE 已 init
static uint8_t s_addr_type;
static uint16_t s_status_handle;
static char s_device_name[24];

static ble_uuid_any_t s_uuid_service;
static ble_uuid_any_t s_uuid_cmd;
static ble_uuid_any_t s_uuid_status;
static ble_uuid_any_t s_uuid_info;
static struct ble_gatt_chr_def s_chrs[4];
static struct ble_gatt_svc_def s_svcs[2];

static int gap_event(struct ble_gap_event *event, void *arg);
static void advertise(void);

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static const char *state_name(pet_provision_state_t state)
{
    switch (state) {
    case PET_PROV_OFF:         return "off";
    case PET_PROV_ADVERTISING: return "advertising";
    case PET_PROV_CONNECTED:   return "connected";
    case PET_PROV_PAIRED:      return "paired";
    case PET_PROV_APPLYING:    return "applying";
    case PET_PROV_DONE:        return "done";
    case PET_PROV_FAILED:      return "failed";
    }
    return "?";
}

// 通知界面。回调在调用方所在的上下文里跑(宿主任务或配网任务), 所以 pet_app
// 那边的实现必须能容忍这两个线程 —— 与 pet_bridge 的回调约定一致。
static void set_state(pet_provision_state_t state, const char *detail)
{
    char snapshot[sizeof(s_detail)];

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_state = state;
    snprintf(s_detail, sizeof(s_detail), "%s", detail != NULL ? detail : "");
    snprintf(snapshot, sizeof(snapshot), "%s", s_detail);
    xSemaphoreGive(s_state_lock);

    ESP_LOGI(TAG, "状态 %s%s%s", state_name(state), snapshot[0] != '\0' ? " " : "",
             snapshot);
    if (s_sink_valid && s_sink.on_state != NULL) {
        s_sink.on_state(state, snapshot[0] != '\0' ? snapshot : NULL, s_sink.user);
    }
}

static void random_pin(char *out, size_t size)
{
    // 1000..9999: 避开前导零, 免得读出"0123"这种容易被听错的码。
    snprintf(out, size, "%u", (unsigned)(1000u + esp_random() % 9000u));
}

// 生成一个新码并落盘。两个调用点:
//   1. 每次开配网页(2026-09-20 起): 配对码要防的只是"别的机器顺手连上", 而它明文
//      打进串口日志、长期躺在 NVS 里 —— 长期码等于 once 泄漏、永远可配。用户每次
//      配网都得看屏幕抄码, 换码不添事, 换来的是所有旧码(见过/抄过/日志里)作废。
//   2. 配对码被连续试错耗尽: 那不是手滑, 是有人在试, 立刻换码作废对方手里的旧码。
static void rotate_pin(void)
{
    char pin[PET_SETTINGS_PIN_MAX];
    random_pin(pin, sizeof(pin));

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    snprintf(s_pin, sizeof(s_pin), "%s", pin);
    s_attempts_left = PET_PROVISION_MAX_ATTEMPTS;
    xSemaphoreGive(s_state_lock);

    // NVS 写在锁外。存不下来只影响"下一个窗口", 本次屏幕上的码照样有效。
    const esp_err_t err = pet_settings_save_pin(pin);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "新配对码 %s 已生成并保存", pin);
    } else {
        ESP_LOGW(TAG, "新配对码 %s 保存失败(%s), 本次仍然可用",
                 pin, esp_err_to_name(err));
    }
}

// ---------------------------------------------------------------------------
// 状态上报(宿主任务里真正发出)
// ---------------------------------------------------------------------------
static void notify_callout(struct ble_npl_event *event)
{
    (void)event;

    if (!atomic_load(&s_connected)) return;
    const uint16_t handle = (uint16_t)atomic_load(&s_conn_handle);
    if (handle == BLE_HS_CONN_HANDLE_NONE) return;

    char line[PET_PROV_LINE_MAX];
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    snprintf(line, sizeof(line), "%s", s_pending_status);
    xSemaphoreGive(s_tx_lock);
    if (line[0] == '\0') return;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(line, (uint16_t)strlen(line));
    if (om == NULL) {
        ESP_LOGW(TAG, "状态报文分配失败");
        return;
    }
    // 成功失败都好: ble_gatts_notify_custom 自己会回收 om。
    (void)ble_gatts_notify_custom(handle, s_status_handle, om);
}

// 上报一条状态。可以从任意任务调用。
static void publish_status(const char *event, const char *message,
                           const char *field, const char *value)
{
    char line[PET_PROV_LINE_MAX];
    if (!pet_prov_status_json(line, sizeof(line), event, message, field, value)) {
        ESP_LOGW(TAG, "状态报文放不下, 已丢弃: %s", event);
        return;
    }

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    snprintf(s_pending_status, sizeof(s_pending_status), "%s", line);
    snprintf(s_last_status, sizeof(s_last_status), "%s", line);
    const bool ready = s_ble_ready;
    if (ready) {
        // 0 ticks = 尽快在宿主任务里执行。
        (void)ble_npl_callout_reset(&s_notify_callout, 0);
    }
    xSemaphoreGive(s_tx_lock);

    ESP_LOGI(TAG, "上报 %s", line);
}

static bool build_info_json(char *out, size_t size)
{
    pet_settings_t current;
    bool paired;

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    current = s_current;
    paired = atomic_load(&s_paired);
    xSemaphoreGive(s_state_lock);

    // SSID 与主机名都来自用户输入, 可能带引号或反斜杠, 必须转义后再拼。
    char ssid[PET_SETTINGS_SSID_MAX * 2];
    char host[PET_SETTINGS_HOST_MAX * 2];
    if (!pet_prov_escape_json(current.ssid, ssid, sizeof(ssid)) ||
        !pet_prov_escape_json(current.host, host, sizeof(host))) {
        return false;
    }

    const int written = snprintf(
        out, size,
        "{\"device\":\"%s\",\"fw\":\"%s\",\"pet\":\"%s\",\"configured\":%s,"
        "\"ssid\":\"%s\",\"host\":\"%s\",\"port\":%u,\"paired\":%s}",
        s_device_name, PET_FIRMWARE_VERSION, pet_slot_pet_id(),
        pet_settings_is_configured(&current) ? "true" : "false", ssid, host,
        (unsigned)current.port, paired ? "true" : "false");

    // 密码永远不出现在任何报文里 —— 只回读 SSID 和配对端。
    return written > 0 && (size_t)written < size;
}

// ---------------------------------------------------------------------------
// GATT
// ---------------------------------------------------------------------------
static int on_cmd_write(struct ble_gatt_access_ctxt *ctxt);
static int on_status_read(struct ble_gatt_access_ctxt *ctxt);
static int on_info_read(struct ble_gatt_access_ctxt *ctxt);

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;

    const struct ble_gatt_chr_def *chr = (const struct ble_gatt_chr_def *)arg;
    if (chr == &s_chrs[0]) return on_cmd_write(ctxt);
    if (chr == &s_chrs[1]) return on_status_read(ctxt);
    if (chr == &s_chrs[2]) return on_info_read(ctxt);
    return BLE_ATT_ERR_UNLIKELY;
}

static int on_status_read(struct ble_gatt_access_ctxt *ctxt)
{
    char line[PET_PROV_LINE_MAX];
    size_t length;

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    length = strlen(s_last_status);
    memcpy(line, s_last_status, length + 1);
    xSemaphoreGive(s_tx_lock);

    // 还没上报过任何状态时给一个空对象, 好过让客户端读到一个错误。
    if (length == 0) {
        memcpy(line, "{}", 3);
        length = 2;
    }
    return os_mbuf_append(ctxt->om, line, (uint16_t)length) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int on_info_read(struct ble_gatt_access_ctxt *ctxt)
{
    char line[PET_PROV_LINE_MAX];
    if (!build_info_json(line, sizeof(line))) return BLE_ATT_ERR_INSUFFICIENT_RES;
    return os_mbuf_append(ctxt->om, line, (uint16_t)strlen(line)) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// ---------------------------------------------------------------------------
// 命令处理(运行在 NimBLE 宿主任务里)
// ---------------------------------------------------------------------------
static void handle_line(const char *line)
{
    pet_prov_parse_t parsed;
    const char *error = NULL;

    if (!pet_prov_parse_line(line, &parsed, &error)) {
        const char *reason = error != NULL ? error : "bad json";
        ESP_LOGW(TAG, "拒绝命令: %s (原始内容 %.32s)", reason, line);
        publish_status("error", reason, NULL, NULL);
        return;
    }

    switch (parsed.cmd) {
    case PET_PROV_CMD_NONE:
        return;

    case PET_PROV_CMD_PIN: {
        if (!pet_prov_pin_is_valid(parsed.text)) {
            publish_status("error", "pin format", NULL, NULL);
            return;
        }
        const int left = s_attempts_left - 1;
        if (strcmp(parsed.text, s_pin) != 0) {
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            s_attempts_left = left;
            xSemaphoreGive(s_state_lock);

            char left_text[12];
            snprintf(left_text, sizeof(left_text), "%d", left);
            publish_status("pin_error", NULL, "left", left_text);

            if (left <= 0) {
                ESP_LOGW(TAG, "配对码连续输错 %d 次, 断开链路",
                         PET_PROVISION_MAX_ATTEMPTS);
                // 换码必须在这里显式做: 断开本身不换码(同一窗口重连应当沿用屏幕
                // 上的码), 只靠断开的话, 这个上限就名存实亡了。
                rotate_pin();
                (void)ble_gap_terminate((uint16_t)atomic_load(&s_conn_handle),
                                        BLE_ERR_REM_USER_CONN_TERM);
            }
            return;
        }

        atomic_store(&s_paired, true);
        set_state(PET_PROV_PAIRED, NULL);
        publish_status("paired", NULL, NULL, NULL);
        return;
    }

    case PET_PROV_CMD_SSID:
    case PET_PROV_CMD_PASS:
    case PET_PROV_CMD_HOST:
    case PET_PROV_CMD_PORT: {
        // 没有配对就先改参数没有意义, 而且会让"配对码"这一层形同虚设。
        if (!atomic_load(&s_paired)) {
            publish_status("error", "not paired", NULL, NULL);
            return;
        }

        const char *key;
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        switch (parsed.cmd) {
        case PET_PROV_CMD_SSID:
            key = "ssid";
            // strlcpy 而不是 snprintf: 这两个缓冲都比源串窄(解析层已经按各自的上限
            // 校验过), 显式截断语义才不会让 -Wformat-truncation 把整个构建挡住。
            strlcpy(s_draft.ssid, parsed.text, sizeof(s_draft.ssid));
            break;
        case PET_PROV_CMD_PASS:
            key = "pass";
            strlcpy(s_draft.password, parsed.text, sizeof(s_draft.password));
            break;
        case PET_PROV_CMD_HOST:
            key = "host";
            strlcpy(s_draft.host, parsed.text, sizeof(s_draft.host));
            break;
        default:
            key = "port";
            s_draft.port = (uint16_t)parsed.number;
            break;
        }
        xSemaphoreGive(s_state_lock);

        // 只回键名, 绝不回显值 —— 密码不能出现在通知里。
        publish_status("set", NULL, "key", key);
        return;
    }

    case PET_PROV_CMD_COMMIT: {
        if (!atomic_load(&s_paired)) {
            publish_status("failed", "not paired", NULL, NULL);
            return;
        }

        pet_settings_t request;
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        request = s_draft;
        xSemaphoreGive(s_state_lock);

        if (request.ssid[0] == '\0') {
            publish_status("failed", "ssid missing", NULL, NULL);
            return;
        }

        set_state(PET_PROV_APPLYING, NULL);
        publish_status("applying", NULL, NULL, NULL);

        // 保存 + 联网可能要几十秒(等 DHCP), 绝不能压在宿主任务上, 转给配网任务。
        if (xQueueOverwrite(s_apply_queue, &request) != pdTRUE) {
            publish_status("failed", "busy", NULL, NULL);
        }
        return;
    }

    case PET_PROV_CMD_FORGET: {
        if (!atomic_load(&s_paired)) {
            publish_status("error", "not paired", NULL, NULL);
            return;
        }
        const esp_err_t err = pet_settings_clear();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "清空存档失败: %s", esp_err_to_name(err));
            publish_status("failed", "forget failed", NULL, NULL);
            return;
        }
        pet_settings_t empty;
        memset(&empty, 0, sizeof(empty));
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        s_draft = empty;
        s_current = empty;
        xSemaphoreGive(s_state_lock);
        ESP_LOGI(TAG, "已清空 NVS 里的网络参数");
        publish_status("forgot", NULL, NULL, NULL);
        return;
    }

    case PET_PROV_CMD_REJECTED:
    default:
        publish_status("error", "unsupported", NULL, NULL);
        return;
    }
}

// 把接收缓冲里所有完整行切出来处理。只在宿主任务里调用。
static void feed_rx(void)
{
    for (;;) {
        char *newline = memchr(s_rx, '\n', s_rx_len);
        if (newline == NULL) {
            // 没有换行: 要么客户端还没写完, 要么它忘了发。缓冲满了直接丢掉,
            // 否则后面所有命令都会错位。
            if (s_rx_len >= sizeof(s_rx)) {
                ESP_LOGW(TAG, "接收缓冲已满且无换行, 丢弃 %u 字节",
                         (unsigned)s_rx_len);
                s_rx_len = 0;
            }
            return;
        }

        size_t line_len = (size_t)(newline - s_rx);
        while (line_len > 0 && s_rx[line_len - 1] == '\r') line_len--;

        char line[PET_PROV_LINE_MAX];
        if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
        memcpy(line, s_rx, line_len);
        line[line_len] = '\0';

        const size_t consumed = (size_t)(newline - s_rx) + 1;
        memmove(s_rx, s_rx + consumed, s_rx_len - consumed);
        s_rx_len -= consumed;

        handle_line(line);
    }
}

static int on_cmd_write(struct ble_gatt_access_ctxt *ctxt)
{
    const uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
    if (length == 0) return 0;

    if (length > PROV_CHUNK_MAX) {
        ESP_LOGW(TAG, "单次写入 %u 字节, 超过上限, 丢弃", (unsigned)length);
        s_rx_len = 0;
        return 0;   // 不返回错误: 那会让客户端直接断开, 反而更难查
    }

    char chunk[PROV_CHUNK_MAX];
    if (os_mbuf_copydata(ctxt->om, 0, length, chunk) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (s_rx_len + length > sizeof(s_rx)) {
        ESP_LOGW(TAG, "接收缓冲溢出, 丢弃整段");
        s_rx_len = 0;
        return 0;
    }
    memcpy(s_rx + s_rx_len, chunk, length);
    s_rx_len += length;

    feed_rx();
    return 0;
}

// ---------------------------------------------------------------------------
// 配网任务: 只负责"保存 + 联网"这段会阻塞的活, 以及看门狗式的窗口超时
// ---------------------------------------------------------------------------
// 返回 true 表示这次配网成功 —— 调用方据此决定要不要关窗口。
static bool apply_request(const pet_settings_t *request)
{
    // 密码不写日志, 与 pet_settings 的约定一致。
    ESP_LOGI(TAG, "收到参数: SSID=\"%s\", 配对端 %s:%u", request->ssid,
             request->host, (unsigned)request->port);

    const esp_err_t saved = pet_settings_save(request);
    if (saved != ESP_OK) {
        ESP_LOGE(TAG, "写入 NVS 失败: %s", esp_err_to_name(saved));
        set_state(PET_PROV_FAILED, "save failed");
        publish_status("failed", "save failed", NULL, NULL);
        return false;
    }

    char ip[20] = { 0 };
    const esp_err_t err = s_sink.apply != NULL
                              ? s_sink.apply(request, ip, sizeof(ip))
                              : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) {
        // 参数已经落盘了, 下次开机会照它重试, 所以这里只是"这次没连上"。
        ESP_LOGW(TAG, "按新参数联网失败: %s", esp_err_to_name(err));
        set_state(PET_PROV_FAILED, "connect failed");
        publish_status("failed", "connect failed", NULL, NULL);
        return false;
    }

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_current = *request;
    xSemaphoreGive(s_state_lock);
    atomic_store(&s_paired, false);

    ESP_LOGI(TAG, "配网成功, 本机地址 %s", ip);
    set_state(PET_PROV_DONE, ip);
    publish_status("done", NULL, "ip", ip);
    return true;
}

static void provision_task(void *arg)
{
    (void)arg;

    const int64_t window_deadline =
        esp_timer_get_time() + (int64_t)PET_PROVISION_WINDOW_MS * 1000;
    int64_t close_at = 0;

    while (s_running) {
        pet_settings_t request;
        if (xQueueReceive(s_apply_queue, &request, pdMS_TO_TICKS(200)) == pdTRUE) {
            // 只有成功才安排关窗。失败就把错误留在屏幕上, 用户还能看一眼再重试
            // (或者等窗口超时), 而不是刚显示"配网失败"就跳回宠物。
            if (apply_request(&request)) {
                close_at = esp_timer_get_time() +
                           (int64_t)PROV_CLOSE_AFTER_DONE_MS * 1000;
                ESP_LOGI(TAG, "配网成功, %d 秒后自动退出配网界面",
                         PROV_CLOSE_AFTER_DONE_MS / 1000);
            }
        }

        if (!s_running) break;

        if (close_at != 0) {
            if (esp_timer_get_time() >= close_at) {
                ESP_LOGI(TAG, "配网完成, 自动退出配网界面");
                break;
            }
        } else if (esp_timer_get_time() >= window_deadline) {
            ESP_LOGI(TAG, "配网窗口超时(%d 秒), 自动退出配网界面",
                     PET_PROVISION_WINDOW_MS / 1000);
            break;
        }
    }

    // 收尾一定要走 pet_provision_stop(): 只删任务的话 s_running 会一直是 true,
    // 界面也收不到 PET_PROV_OFF, 配网画面就永远盖在宠物上面出不来。
    // 它认得出"在自己任务里调用", 因此不会等自己退出。
    (void)pet_provision_stop();

    s_task = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// NimBLE 生命周期
// ---------------------------------------------------------------------------
static void build_gatt_defs(void)
{
    memset(s_chrs, 0, sizeof(s_chrs));
    memset(s_svcs, 0, sizeof(s_svcs));

    // cmd: 唯一可写的特征。WRITE_NO_RSP 也开着, 客户端可以用不带应答的写来提速。
    s_chrs[0].uuid = &s_uuid_cmd.u;
    s_chrs[0].access_cb = gatt_access;
    s_chrs[0].arg = &s_chrs[0];
    s_chrs[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP;

    // status: 通知 + 可读。可读是为了让晚连上来的客户端能直接拿到最后一次状态。
    s_chrs[1].uuid = &s_uuid_status.u;
    s_chrs[1].access_cb = gatt_access;
    s_chrs[1].arg = &s_chrs[1];
    s_chrs[1].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY;
    s_chrs[1].val_handle = &s_status_handle;

    s_chrs[2].uuid = &s_uuid_info.u;
    s_chrs[2].access_cb = gatt_access;
    s_chrs[2].arg = &s_chrs[2];
    s_chrs[2].flags = BLE_GATT_CHR_F_READ;

    // 数组以 uuid == NULL 结尾, memset 已经保证了这一点。

    s_svcs[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    s_svcs[0].uuid = &s_uuid_service.u;
    s_svcs[0].characteristics = s_chrs;
}

static void advertise(void)
{
    if (!s_running) return;

    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &s_uuid_service.u128;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "广播数据设置失败: %d", rc);
        return;
    }

    // 设备名放扫描响应里: 128 位 UUID 已经占掉 21 字节, 31 字节的广播包塞不下名字。
    // 主机做主动扫描时会拿到这段。
    struct ble_hs_adv_fields response = { 0 };
    response.name = (const uint8_t *)s_device_name;
    response.name_len = (uint8_t)strlen(s_device_name);
    response.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&response);
    if (rc != 0) {
        ESP_LOGE(TAG, "扫描响应设置失败: %d", rc);
        return;
    }

    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;   // 可连接: 配网必须能建立连接
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event,
                           NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "广播启动失败: %d", rc);
        return;
    }

    set_state(PET_PROV_ADVERTISING, NULL);
    ESP_LOGI(TAG, "配网窗口已开: 设备名 %s, 等 Mac 连接", s_device_name);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "连接建立失败(0x%02x), 继续广播", event->connect.status);
            advertise();
            return 0;
        }
        atomic_store(&s_conn_handle, event->connect.conn_handle);
        atomic_store(&s_connected, true);
        atomic_store(&s_paired, false);
        // 这里既不换码也不重置尝试次数: 码是本次开窗新生成的(见 rotate_pin), 重连
        // 沿用屏幕上的同一个码; 但连接不白送次数 —— "稳定的码 + 每次连接白送三次"
        // 等于可以把码慢慢试出来。
        {
            char pin[PET_SETTINGS_PIN_MAX];
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            snprintf(pin, sizeof(pin), "%s", s_pin);
            const int left = s_attempts_left;
            xSemaphoreGive(s_state_lock);
            ESP_LOGI(TAG, "Mac 已连接, 配对码 %s, 还剩 %d 次", pin, left);
        }
        set_state(PET_PROV_CONNECTED, NULL);
        publish_status("connected", NULL, NULL, NULL);
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Mac 断开(0x%02x)", event->disconnect.reason);
        atomic_store(&s_connected, false);
        atomic_store(&s_paired, false);
        atomic_store(&s_conn_handle, BLE_HS_CONN_HANDLE_NONE);
        s_rx_len = 0;   // 半条命令不能留到下次连接
        if (s_running) {
            // 断开也沿用同一个码: Mac 那边可能只是蓝牙抖了一下, 换码会让它
            // 拿着屏幕上的旧码一直配不上。
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        // 正常情况是 BLE_HS_FOREVER, 走到这里说明被主动停了或出错。
        if (s_running) advertise();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "协商 MTU = %u", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE 复位, reason=%d", reason);
    atomic_store(&s_connected, false);
    atomic_store(&s_paired, false);
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 ||
        ble_hs_id_infer_auto(0, &s_addr_type) != 0) {
        ESP_LOGE(TAG, "无法确定蓝牙地址类型, 配网不可用");
        return;
    }
    advertise();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void teardown_nimble(void)
{
    if (!s_initialized) return;

    // 先关掉上报入口: 之后配网任务即使还在跑也不会再碰已经销毁的 callout。
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    s_ble_ready = false;
    xSemaphoreGive(s_tx_lock);

    (void)ble_gap_adv_stop();
    (void)ble_gap_terminate(BLE_HS_CONN_HANDLE_NONE, BLE_ERR_REM_USER_CONN_TERM);
    if (nimble_port_stop() != 0) {
        ESP_LOGW(TAG, "nimble_port_stop 失败");
    }
    (void)nimble_port_deinit();

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    ble_npl_callout_deinit(&s_notify_callout);
    xSemaphoreGive(s_tx_lock);

    s_initialized = false;
    atomic_store(&s_connected, false);
    atomic_store(&s_paired, false);
    atomic_store(&s_conn_handle, BLE_HS_CONN_HANDLE_NONE);
}

// ---------------------------------------------------------------------------
// 对外 API
// ---------------------------------------------------------------------------
esp_err_t pet_provision_start(const pet_provision_sink_t *sink,
                              const pet_settings_t *current)
{
    if (sink == NULL || sink->apply == NULL) return ESP_ERR_INVALID_ARG;

    if (s_running) {
        // 窗口已经开着: 只换回调, 不动 BLE。
        s_sink = *sink;
        s_sink_valid = true;
        return ESP_OK;
    }

    // 演示菜单里的 BLE 示例与配网共用同一个 NimBLE 栈, 不能同时初始化。
    if (s_initialized) {
        ESP_LOGW(TAG, "蓝牙已被占用(演示菜单?), 无法打开配网窗口");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state_lock == NULL) {
        s_state_lock = xSemaphoreCreateMutex();
        if (s_state_lock == NULL) return ESP_ERR_NO_MEM;
    }
    if (s_tx_lock == NULL) {
        s_tx_lock = xSemaphoreCreateMutex();
        if (s_tx_lock == NULL) return ESP_ERR_NO_MEM;
    }
    if (s_apply_queue == NULL) {
        s_apply_queue = xQueueCreate(1, sizeof(pet_settings_t));
        if (s_apply_queue == NULL) return ESP_ERR_NO_MEM;
    }

    // nimble_port_init() 必须排在 UUID 解析之前。
    //
    // ble_uuid_from_str() 内部会走 ble_uuid_base_init(), 而它用 NimBLE 的内存分配器
    // 申请 16 字节来放蓝牙基 UUID。分配器在 port 初始化之前还没就绪, 会返回
    // BLE_HS_ENOMEM —— 于是四个 UUID 全都"解析失败", 报出来像是把 UUID 字符串写错
    // 了, 实际是调用顺序问题。而且它是时好时坏的: 同样一份固件, 有的开机成功, 有的
    // 开机失败(实测就撞上过), 所以顺序不能讲运气。
    const esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE 初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    s_initialized = true;

    // 逐个解析并记下具体是哪一个、返回码是多少。合成一个布尔判断会把这些信息全丢掉,
    // 而这里的失败只可能在真机上才暴露(主机测试碰不到 NimBLE), 所以诊断信息要留够。
    struct {
        ble_uuid_any_t *out;
        const char     *text;
    } const uuid_specs[] = {
        { &s_uuid_service, PROV_UUID_SERVICE },
        { &s_uuid_cmd,     PROV_UUID_CMD     },
        { &s_uuid_status,  PROV_UUID_STATUS  },
        { &s_uuid_info,    PROV_UUID_INFO    },
    };
    for (size_t i = 0; i < sizeof(uuid_specs) / sizeof(uuid_specs[0]); i++) {
        const int rc = ble_uuid_from_str(uuid_specs[i].out, uuid_specs[i].text);
        if (rc != 0) {
            ESP_LOGE(TAG, "UUID 解析失败: %s (rc=%d)", uuid_specs[i].text, rc);
            teardown_nimble();
            return ESP_FAIL;
        }
    }
    build_gatt_defs();

    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "统计 GATT 配置失败: %d", rc);
        teardown_nimble();
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "注册 GATT 服务失败: %d", rc);
        teardown_nimble();
        return ESP_FAIL;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
        snprintf(s_device_name, sizeof(s_device_name), "CodexPet-%02X%02X", mac[4],
                 mac[5]);
    } else {
        snprintf(s_device_name, sizeof(s_device_name), "CodexPet");
    }
    if (ble_svc_gap_device_name_set(s_device_name) != 0) {
        ESP_LOGW(TAG, "设置设备名失败, 客户端可能只看到 MAC");
    }

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    (void)ble_npl_callout_init(&s_notify_callout, nimble_port_get_dflt_eventq(),
                               notify_callout, NULL);
    s_last_status[0] = '\0';
    s_pending_status[0] = '\0';
    s_ble_ready = true;
    xSemaphoreGive(s_tx_lock);

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_current = current != NULL ? *current : (pet_settings_t){ 0 };
    // 草稿从当前配置起头: Mac 只要改要改的字段, 不必先把所有项都填一遍。
    s_draft = s_current;
    if (s_draft.port == 0) s_draft.port = (uint16_t)PET_BRIDGE_PORT;
    xSemaphoreGive(s_state_lock);

    s_sink = *sink;
    s_sink_valid = true;
    s_pin[0] = '\0';
    s_rx_len = 0;
    s_running = true;
    rotate_pin();   // 每次开配网页都换新码(见 rotate_pin 上的注释)

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);

    if (xTaskCreate(provision_task, "pet_provision", 4096, NULL, 3, &s_task) !=
        pdPASS) {
        ESP_LOGE(TAG, "配网任务创建失败");
        pet_provision_stop();
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t pet_provision_stop(void)
{
    if (!s_running && !s_initialized) return ESP_OK;

    const bool from_task = (s_task != NULL && s_task == xTaskGetCurrentTaskHandle());
    s_running = false;

    teardown_nimble();

    // 等配网任务收尾。可能正卡在 apply(等 IP)里, 所以给足时间但仍有界。
    if (!from_task) {
        for (int i = 0; i < 200 && s_task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (s_task != NULL) {
            ESP_LOGW(TAG, "配网任务未在预期时间内退出");
        }
    }

    // 先把 OFF 发出去, 再作废 sink —— 顺序反了这条通知就永远送不到界面:
    // set_state() 只在 sink 有效时才回调(main/pet_provision.c 的 set_state), 而
    // pet_app 靠这条通知收起配网页、并放开"配网页期间不自动息屏"的限制。作废放在
    // 后面照样能挡住 stop() 之后的杂散回调。
    set_state(PET_PROV_OFF, NULL);
    s_sink_valid = false;
    return ESP_OK;
}

bool pet_provision_active(void)
{
    return s_running;
}

const char *pet_provision_device_name(void)
{
    return s_device_name;
}

bool pet_provision_pin(char *out, size_t size)
{
    if (out == NULL || size == 0) return false;
    if (s_state_lock == NULL) {
        out[0] = '\0';
        return false;
    }

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    const bool valid = s_pin[0] != '\0';
    snprintf(out, size, "%s", s_pin);
    xSemaphoreGive(s_state_lock);
    return valid;
}
