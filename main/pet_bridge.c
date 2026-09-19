// main/pet_bridge.c
#include "pet_bridge.h"

#include <errno.h>
#include <fcntl.h>
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

static const char *TAG = "pet_bridge";

#define BIT_GOT_IP  BIT0
#define BIT_STOPPED BIT1

#define PET_BRIDGE_TASK_STACK 5120
#define PET_BRIDGE_TASK_PRIORITY 5
// 单次 select 的等待上限, 决定了对 stop 请求的响应速度。
#define PET_BRIDGE_SELECT_TIMEOUT_MS 500
// 一次最多处理多少接收字节。
#define PET_BRIDGE_RX_CHUNK 512

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

// 这些函数由 bridge / 事件任务调用, 因此把回调包一层, 保证 sink 已经设置好。
static void emit_link(pet_link_state_t link)
{
    if (s_sink_valid && s_sink.on_link) s_sink.on_link(link, s_sink.user);
}

// 签名要匹配 pet_msg_cb_t(第二个参数是 user, 这里不用), 否则类型不兼容。
static void emit_message(const pet_message_t *msg, void *user)
{
    (void)user;
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
    char line[PET_PROTOCOL_LINE_MAX];
    snprintf(line, sizeof(line),
             "{\"type\":\"hello\",\"fw\":\"%s\",\"pet\":\"%s\"}\n",
             PET_FIRMWARE_VERSION, PET_PACKAGE_ID);
    if (send_line(line) != ESP_OK) {
        ESP_LOGW(TAG, "hello 发送失败");
    }
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

// 返回 true 表示链路仍然健康。
static bool pump_socket(pet_protocol_t *protocol, int64_t *last_rx_us)
{
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
        return (esp_timer_get_time() - *last_rx_us) <
               (int64_t)PET_BRIDGE_IDLE_TIMEOUT_MS * 1000;
    }

    const ssize_t received = recv(s_socket, buffer, sizeof(buffer), 0);
    if (received > 0) {
        *last_rx_us = esp_timer_get_time();
        pet_protocol_feed(protocol, buffer, (size_t)received, emit_message, NULL);
        return true;
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
        last_rx_us = esp_timer_get_time();
        // 必须先置 online 再发 hello: send_line() 会用 s_online 判断链路是否可用,
        // 顺序反了 hello 会被自己挡掉(实测就是这条一直发不出去)。
        set_online(true);
        send_hello();

        while (!atomic_load(&s_stop) && generation == s_generation) {
            if (!pump_socket(&protocol, &last_rx_us)) break;
        }

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
