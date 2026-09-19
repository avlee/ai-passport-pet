// main/pet_bridge.c
#include "pet_bridge.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "pet_config.h"
#include "pet_pkg.h"
#include "pet_slot.h"

static const char *TAG = "pet_bridge";

#define BIT_GOT_IP  BIT0
#define BIT_STOPPED BIT1

#define PET_BRIDGE_TASK_STACK 5120
#define PET_BRIDGE_TASK_PRIORITY 5
// 单次 select 的等待上限, 决定了对 stop 请求的响应速度。
#define PET_BRIDGE_SELECT_TIMEOUT_MS 500
// 一次最多处理多少接收字节。传输宠物包时这里就是每次 recv 的块大小, 所以别太小。
#define PET_BRIDGE_RX_CHUNK 1024

static pet_settings_t s_settings;
static pet_bridge_sink_t s_sink;
static bool s_sink_valid;

static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_tx_lock;
static TaskHandle_t s_task;
static esp_netif_t *s_netif;
static int s_socket = -1;
static atomic_bool s_stop;
static atomic_bool s_online;
static bool s_ready;
static uint32_t s_generation;  // 每次 start/stop 递增, 用于让旧任务自行退出

// ---------------------------------------------------------------------------
// 宠物包接收
// ---------------------------------------------------------------------------
// 线路上的形态: 一行 {"type":"pet","id":...,"size":N,"crc32":...} 之后, 紧跟 N 个
// **裸字节**。不套 base64 是因为 3 MB 的包多出 33% 的传输量, 而这段是整条链路最
// 慢的一步; 直接流式写进 flash 也省掉一次完整的 RAM 缓冲(那点堆根本不够)。
//
// 代价是"行模式"和"二进制模式"要在同一根流上切换, 而两者可能落在同一个 recv 里,
// 所以切换到二进制之后剩下的字节必须按计数消费, 不能再喂给行解析器。
typedef struct {
    bool     active;
    bool     discard;    // 应用拒收(槽放不下/正在配网): 读完丢掉, 只为保持同步
    uint32_t remaining;  // 还差多少字节
    uint32_t total;
    char     id[PET_PKG_ID_MAX];
} pet_rx_t;

static pet_rx_t s_pet_rx;
// 流里还夹着没读完的二进制, 却已经收到了下一个宣告 —— 两边对不上了, 没法就地
// 重新同步, 只能断开重连。置位后 pump_socket() 立刻返回假。
static bool s_rx_desync;

// 这些函数由 bridge / 事件任务调用, 因此把回调包一层, 保证 sink 已经设置好。
static void emit_link(pet_link_state_t link)
{
    if (s_sink_valid && s_sink.on_link) s_sink.on_link(link, s_sink.user);
}

static bool pet_data_sink_ready(void)
{
    return s_sink_valid && !s_pet_rx.discard && s_sink.on_pet_data != NULL;
}

// 处理一条 {"type":"pet",...} 宣告。**不把它转发给 on_message**: 应用要从
// on_pet_begin/on_pet_data/on_pet_end 这条路径感知传输, 多一条消息只会让人以为
// 有两个数据源。
static void arm_pet_rx(const pet_message_t *msg)
{
    if (s_pet_rx.active) {
        ESP_LOGE(TAG, "上一份宠物包还差 %u 字节没读完, 断开链路重新对齐",
                 (unsigned)s_pet_rx.remaining);
        s_rx_desync = true;
        return;
    }

    const uint32_t size = msg->pet_size;
    bool accepted = false;

    if (size > PET_BRIDGE_PET_MAX_BYTES) {
        ESP_LOGW(TAG, "宠物包声明 %u 字节, 超过上限 %u, 拒收", (unsigned)size,
                 (unsigned)PET_BRIDGE_PET_MAX_BYTES);
    } else {
        if (s_sink_valid && s_sink.on_pet_begin != NULL) {
            accepted = s_sink.on_pet_begin(msg->text, size, msg->pet_crc32,
                                          s_sink.user) == ESP_OK;
        }
        if (!accepted) ESP_LOGW(TAG, "应用拒收宠物包 %s(%u 字节), 收完即丢",
                                msg->text, (unsigned)size);
    }

    s_pet_rx.active = true;
    s_pet_rx.discard = !accepted;
    s_pet_rx.remaining = size;
    s_pet_rx.total = size;
    // 解析器已经拒过长过 PET_PKG_ID_MAX 的 id, 这里用 strlcpy 只是为了不触发
    // -Wformat-truncation(编译器看不到另一侧的长度校验)。
    strlcpy(s_pet_rx.id, msg->text, sizeof(s_pet_rx.id));
}

static void finish_pet_rx(bool ok)
{
    if (!s_pet_rx.active) return;

    s_pet_rx.active = false;
    s_pet_rx.remaining = 0;

    if (!s_pet_rx.discard && s_sink_valid && s_sink.on_pet_end != NULL) {
        s_sink.on_pet_end(ok, s_sink.user);
    }
    s_pet_rx.discard = false;
}

// 签名要匹配 pet_msg_cb_t(第二个参数是 user, 这里不用), 否则类型不兼容。
static void emit_message(const pet_message_t *msg, void *user)
{
    (void)user;
    if (msg->type == PET_MSG_PET) {
        arm_pet_rx(msg);
        return;
    }
    if (s_sink_valid && s_sink.on_message) s_sink.on_message(msg, s_sink.user);
}

// ---------------------------------------------------------------------------
// Wi-Fi 事件
// ---------------------------------------------------------------------------

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id,
                               void *data)
{
    (void)arg;
    (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // 凭据错误会在这里反复失败, 由 bridge 任务按退避节奏重试。
        (void)esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_events) xEventGroupClearBits(s_events, BIT_GOT_IP);
        if (atomic_exchange(&s_online, false)) {
            ESP_LOGW(TAG, "Wi-Fi 断开, 宠物进入睡眠");
            emit_link(PET_LINK_OFFLINE);
        }
        // 退避重连由任务负责节奏控制, 这里只做一次立即重试。
        if (!atomic_load(&s_stop)) (void)esp_wifi_connect();
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "获取 IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (s_events) xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

// ---------------------------------------------------------------------------
// TCP 收发
// ---------------------------------------------------------------------------

// 在最多 timeout_ms 内把 buf 全部写出。socket 是非阻塞的, 所以用 select 等待。
static bool socket_write_all(const char *buf, size_t len, int timeout_ms)
{
    size_t sent = 0;
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (sent < len) {
        if (atomic_load(&s_stop)) return false;
        const int64_t remaining_us = deadline - esp_timer_get_time();
        if (remaining_us <= 0) return false;

        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(s_socket, &write_set);
        struct timeval tv = { .tv_sec = 0,
                              .tv_usec = (suseconds_t)(remaining_us > 200000
                                                           ? 200000
                                                           : remaining_us) };
        const int ready = select(s_socket + 1, NULL, &write_set, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (ready == 0) continue;

        const ssize_t written = send(s_socket, buf + sent, len - sent, 0);
        if (written > 0) {
            sent += (size_t)written;
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        return false;
    }
    return true;
}

// 发送一行。加锁保证多任务并发调用时不会把两行 JSON 交叉写在一起。
static esp_err_t send_line(const char *line)
{
    if (s_socket < 0 || !atomic_load(&s_online)) return ESP_ERR_INVALID_STATE;
    if (s_tx_lock == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const bool ok = socket_write_all(line, strlen(line), 2000);
    xSemaphoreGive(s_tx_lock);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t pet_bridge_notify(const char *type, const char *text)
{
    if (type == NULL) return ESP_ERR_INVALID_ARG;

    char escaped[PET_PROTOCOL_TEXT_MAX * 2];
    char line[PET_PROTOCOL_LINE_MAX];
    if (text != NULL) {
        if (!pet_protocol_escape(text, escaped, sizeof(escaped))) {
            // 转义后放不下就放弃这条文本, 而不是发出被截断的 JSON。
            ESP_LOGW(TAG, "发送的文本过长, 已丢弃");
            text = NULL;
        }
    }

    if (text != NULL) {
        snprintf(line, sizeof(line), "{\"type\":\"%s\",\"text\":\"%s\"}\n", type,
                 escaped);
    } else {
        snprintf(line, sizeof(line), "{\"type\":\"%s\"}\n", type);
    }
    return send_line(line);
}

static void send_hello(void)
{
    // pet 报的是槽里**当前**那只 —— 它现在是运行时的, 换一只不用重刷固件。
    // "none" 是有意义的值: 菜单栏看到它就把默认那只推下来。
    char line[PET_PROTOCOL_LINE_MAX];
    snprintf(line, sizeof(line),
             "{\"type\":\"hello\",\"fw\":\"%s\",\"pet\":\"%s\"}\n",
             PET_FIRMWARE_VERSION, pet_slot_pet_id());
    if (send_line(line) != ESP_OK) {
        ESP_LOGW(TAG, "hello 发送失败");
    }
}

// 最后一次成功读到的电量。INT_MIN = 还没读到过, 重连时不必补发。
static atomic_int s_last_soc = ATOMIC_VAR_INIT(INT_MIN);

static esp_err_t send_battery(int soc)
{
    char line[64];
    if (soc < 0) {
        snprintf(line, sizeof(line), "{\"type\":\"battery\",\"soc\":null}\n");
    } else {
        snprintf(line, sizeof(line), "{\"type\":\"battery\",\"soc\":%d}\n", soc);
    }
    return send_line(line);
}

esp_err_t pet_bridge_notify_battery(int soc_percent)
{
    atomic_store(&s_last_soc, soc_percent);
    return send_battery(soc_percent);
}

esp_err_t pet_bridge_notify_pet_done(bool ok)
{
    // 成功时报的是**槽里那只**(从包头读出来的), 不是宣告里那只 —— 万一对面发错了
    // 文件, 这一条就是菜单栏发现真相的地方。失败时才退回宣告里的 id, 好让它知道
    // 是哪一次尝试失败了。
    const char *id = ok ? pet_slot_pet_id() : s_pet_rx.id;

    // 转义一遍: id 虽然由解析器限了长度, 但里面的引号/反斜杠会把这一行 JSON 拼坏。
    char escaped[PET_PKG_ID_MAX * 2];
    if (!pet_protocol_escape(id, escaped, sizeof(escaped))) {
        escaped[0] = '\0';
    }

    char line[PET_PROTOCOL_LINE_MAX];
    snprintf(line, sizeof(line), "{\"type\":\"petdone\",\"id\":\"%s\",\"ok\":%s}\n",
             escaped, ok ? "true" : "false");
    const esp_err_t err = send_line(line);

    // 换成功了就把新的身份也报一遍: 菜单栏据此更新"现在是哪只", 不必等重连。
    if (ok) send_hello();
    return err;
}

// ---------------------------------------------------------------------------
// 连接流程
// ---------------------------------------------------------------------------

static bool open_socket(void)
{
    char port_text[8];
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)s_settings.port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *result = NULL;
    const int gai = getaddrinfo(s_settings.host, port_text, &hints, &result);
    if (gai != 0 || result == NULL) {
        // lwIP 不提供 gai_strerror(), 直接打返回码; 常见值: -2 名称解析失败,
        // -3 地址族/类型不支持, -4 无地址。
        ESP_LOGW(TAG, "无法解析 %s: getaddrinfo=%d", s_settings.host, gai);
        if (result) freeaddrinfo(result);
        return false;
    }

    s_socket = socket(result->ai_family, result->ai_socktype, 0);
    if (s_socket < 0) {
        ESP_LOGW(TAG, "创建 socket 失败: errno=%d", errno);
        freeaddrinfo(result);
        return false;
    }

    // 连接超时 5 秒: 非阻塞 connect + select 可写。
    const int flags = fcntl(s_socket, F_GETFL, 0);
    fcntl(s_socket, F_SETFL, flags | O_NONBLOCK);

    bool connected = false;
    if (connect(s_socket, result->ai_addr, result->ai_addrlen) == 0) {
        connected = true;
    } else if (errno == EINPROGRESS) {
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(s_socket, &write_set);
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        if (select(s_socket + 1, NULL, &write_set, NULL, &tv) > 0) {
            int socket_error = 0;
            socklen_t length = sizeof(socket_error);
            if (getsockopt(s_socket, SOL_SOCKET, SO_ERROR, &socket_error,
                           &length) == 0 &&
                socket_error == 0) {
                connected = true;
            }
        }
    }
    freeaddrinfo(result);

    if (!connected) {
        close(s_socket);
        s_socket = -1;
        return false;
    }

    // 打开 TCP_NODELAY: 状态同步是低频小包, 不希望被 Nagle 攒批延迟。
    const int one = 1;
    setsockopt(s_socket, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    ESP_LOGI(TAG, "已连接 Pet Bridge %s:%u", s_settings.host,
             (unsigned)s_settings.port);
    return true;
}

static void close_socket(void)
{
    if (s_socket >= 0) {
        shutdown(s_socket, SHUT_RDWR);
        close(s_socket);
        s_socket = -1;
    }
}

// 按当前模式消费一批刚收到的字节。返回 true 表示链路仍然健康。
//
// 关键在"一次只喂一个字节给行解析器": 宣告那一行结束时, 同一批 recv 里往往已经
// 跟着几百字节的二进制载荷, 按整块喂进去它们会被当成文本来解析。逐字节喂没有
// 性能问题 —— 真正占流量的 3 MB 载荷是整块消费的, 走行解析的只有 JSON 行本身。
static bool consume_rx(pet_protocol_t *protocol, const char *data, size_t len)
{
    size_t offset = 0;

    while (offset < len) {
        if (s_pet_rx.active) {
            if (s_pet_rx.remaining == 0) {
                // 不该发生(收满就前移到结束态了), 但真发生的话这里会死循环。
                s_rx_desync = true;
                return false;
            }

            size_t take = len - offset;
            if (take > s_pet_rx.remaining) take = s_pet_rx.remaining;

            if (pet_data_sink_ready()) {
                s_sink.on_pet_data((const uint8_t *)data + offset, take,
                                   s_sink.user);
            }
            offset += take;
            s_pet_rx.remaining -= (uint32_t)take;
            if (s_pet_rx.remaining == 0) finish_pet_rx(true);
            continue;
        }

        pet_protocol_feed(protocol, data + offset, 1, emit_message, NULL);
        offset++;
        if (s_rx_desync) return false;
    }
    return true;
}

// 返回 true 表示链路仍然健康。
static bool pump_socket(pet_protocol_t *protocol, int64_t *last_rx_us)
{
    // 流已经对不上(二进制没读完就来了新宣告), 本地没法重新同步, 交给外层重连。
    if (s_rx_desync) return false;

    char buffer[PET_BRIDGE_RX_CHUNK];

    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(s_socket, &read_set);
    struct timeval tv = { .tv_sec = 0,
                          .tv_usec = PET_BRIDGE_SELECT_TIMEOUT_MS * 1000 };
    const int ready = select(s_socket + 1, &read_set, NULL, NULL, &tv);
    if (ready < 0) {
        if (errno == EINTR) return true;
        ESP_LOGW(TAG, "select 失败: errno=%d", errno);
        return false;
    }
    if (ready == 0) {
        // 对端静默可能是拔网线/睡死; 超过空闲上限就主动断开重连。
        // 传输中不算空闲: 擦 flash 会把这一拍拖长, 而载荷本身不刷新 last_rx_us
        // 之外的东西 —— 这里靠 last_rx_us 就够了, 因为每收到数据都会更新它。
        return (esp_timer_get_time() - *last_rx_us) <
               (int64_t)PET_BRIDGE_IDLE_TIMEOUT_MS * 1000;
    }

    const ssize_t received = recv(s_socket, buffer, sizeof(buffer), 0);
    if (received > 0) {
        *last_rx_us = esp_timer_get_time();
        return consume_rx(protocol, buffer, (size_t)received);
    }
    if (received == 0) {
        ESP_LOGI(TAG, "对端关闭连接");
        return false;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return true;
    ESP_LOGW(TAG, "recv 失败: errno=%d", errno);
    return false;
}

static void set_online(bool online)
{
    if (atomic_exchange(&s_online, online) != online) {
        emit_link(online ? PET_LINK_ONLINE : PET_LINK_OFFLINE);
    }
}

static void bridge_task(void *arg)
{
    const uint32_t generation = (uint32_t)(uintptr_t)arg;
    pet_protocol_t protocol;
    pet_protocol_init(&protocol);

    uint32_t backoff_ms = PET_BRIDGE_RETRY_MIN_MS;
    // 上次提醒过的退避档位。连不上时只在档位变化时打一条, 避免每轮刷屏。
    uint32_t logged_backoff = 0;
    int64_t last_rx_us = esp_timer_get_time();

    // 停止请求可能在任务还没跑起来时就到了, 所以每轮都重新检查。
    while (!atomic_load(&s_stop) && generation == s_generation) {
        emit_link(PET_LINK_CONNECTING);

        // 等 IP。没有 IP 时不该尝试连接, 也不该反复打印解析失败。
        xEventGroupWaitBits(s_events, BIT_GOT_IP, pdFALSE, pdTRUE,
                            pdMS_TO_TICKS(2000));
        if (atomic_load(&s_stop) || generation != s_generation) break;
        if ((xEventGroupGetBits(s_events) & BIT_GOT_IP) == 0) continue;

        if (!open_socket()) {
            if (backoff_ms != logged_backoff) {
                ESP_LOGW(TAG, "连接 %s:%u 失败, %u ms 后重试", s_settings.host,
                         (unsigned)s_settings.port, (unsigned)backoff_ms);
                logged_backoff = backoff_ms;
            }
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = backoff_ms * 2 > PET_BRIDGE_RETRY_MAX_MS
                             ? PET_BRIDGE_RETRY_MAX_MS
                             : backoff_ms * 2;
            continue;
        }

        backoff_ms = PET_BRIDGE_RETRY_MIN_MS;
        logged_backoff = 0;
        pet_protocol_init(&protocol);
        s_rx_desync = false;
        memset(&s_pet_rx, 0, sizeof(s_pet_rx));
        last_rx_us = esp_timer_get_time();
        // 必须先置 online 再发 hello: send_line() 会用 s_online 判断链路是否可用,
        // 顺序反了 hello 会被自己挡掉(实测就是这条一直发不出去)。
        set_online(true);
        send_hello();
        // 补发一次已知电量: 对端刚连上, 手里还没有任何读数, 而电量最多 5 秒
        // 才刷新一次 —— 不补发的话菜单栏会先空一段。
        const int known_soc = atomic_load(&s_last_soc);
        if (known_soc != INT_MIN) {
            (void)send_battery(known_soc);
        }

        while (!atomic_load(&s_stop) && generation == s_generation) {
            if (!pump_socket(&protocol, &last_rx_us)) break;
        }

        // 传到一半断线: 必须通知应用。槽的头部在 begin 时就已经擦掉了, 所以这时
        // 槽里既没有旧宠物也没有新宠物 —— 应用要据此把界面切回"还没有宠物"。
        finish_pet_rx(false);

        set_online(false);
        close_socket();

        if (!atomic_load(&s_stop) && generation == s_generation) {
            ESP_LOGI(TAG, "链路中断, %u ms 后重连", (unsigned)backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = backoff_ms * 2 > PET_BRIDGE_RETRY_MAX_MS
                             ? PET_BRIDGE_RETRY_MAX_MS
                             : backoff_ms * 2;
        }
    }

    ESP_LOGI(TAG, "bridge 任务退出");
    if (s_events) xEventGroupSetBits(s_events, BIT_STOPPED);
    s_task = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// 对外 API
// ---------------------------------------------------------------------------

esp_err_t pet_bridge_prepare(const pet_settings_t *settings)
{
    if (settings == NULL) return ESP_ERR_INVALID_ARG;
    s_settings = *settings;

    if (s_events == NULL) {
        s_events = xEventGroupCreate();
        if (s_events == NULL) return ESP_ERR_NO_MEM;
    }
    if (s_tx_lock == NULL) {
        s_tx_lock = xSemaphoreCreateMutex();
        if (s_tx_lock == NULL) return ESP_ERR_NO_MEM;
    }

    if (s_ready) {
        // 已经初始化过: 只更新凭据并重连。
        wifi_config_t config;
        memset(&config, 0, sizeof(config));
        strlcpy((char *)config.sta.ssid, s_settings.ssid,
                sizeof(config.sta.ssid));
        strlcpy((char *)config.sta.password, s_settings.password,
                sizeof(config.sta.password));
        // 先把"已拿到 IP"清掉: 换凭据之后旧地址就不再有效, 否则配网流程会拿着
        // 上一个网络的地址误判成连接成功。
        xEventGroupClearBits(s_events, BIT_GOT_IP);
        esp_wifi_disconnect();
        esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
        if (err == ESP_OK) err = esp_wifi_connect();
        return err;
    }

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        // 不为了启动网络而擦除用户数据。
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) return ESP_FAIL;

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK) return err;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) return err;

    wifi_config_t config;
    memset(&config, 0, sizeof(config));
    strlcpy((char *)config.sta.ssid, s_settings.ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, s_settings.password,
            sizeof(config.sta.password));
    // 只支持 2.4 GHz; 允许开放网络(密码为空)。
    config.sta.threshold.authmode = s_settings.password[0] == '\0'
                                        ? WIFI_AUTH_OPEN
                                        : WIFI_AUTH_WPA2_PSK;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 启动失败: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    return ESP_OK;
}

esp_err_t pet_bridge_start(const pet_bridge_sink_t *sink)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (sink != NULL) {
        s_sink = *sink;
        s_sink_valid = true;
    }
    if (s_task != NULL) return ESP_OK;

    atomic_store(&s_stop, false);
    s_generation++;
    if (xTaskCreate(bridge_task, "pet_bridge", PET_BRIDGE_TASK_STACK,
                    (void *)(uintptr_t)s_generation, PET_BRIDGE_TASK_PRIORITY,
                    &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t pet_bridge_stop(void)
{
    if (s_task == NULL) {
        close_socket();
        set_online(false);
        return ESP_OK;
    }

    atomic_store(&s_stop, true);
    s_generation++;          // 让任务在下一轮检查时退出
    close_socket();          // 打断阻塞中的 select
    if (s_events) xEventGroupClearBits(s_events, BIT_STOPPED);

    // 任务自己有界退出; 超时说明它卡在不可中断的调用上。
    for (int i = 0; i < 40 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_task != NULL) {
        ESP_LOGW(TAG, "bridge 任务未在预期时间内退出");
        return ESP_ERR_TIMEOUT;
    }

    set_online(false);
    s_sink_valid = false;
    return ESP_OK;
}

bool pet_bridge_is_online(void)
{
    return atomic_load(&s_online);
}

bool pet_bridge_local_ip(char *out, size_t size)
{
    if (out == NULL || size == 0 || s_netif == NULL) return false;

    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_netif, &info) != ESP_OK) return false;
    if (info.ip.addr == 0) return false;   // 还没 DHCP 到地址

    snprintf(out, size, IPSTR, IP2STR(&info.ip));
    return true;
}
