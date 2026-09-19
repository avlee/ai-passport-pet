// main/pet_bridge.h
// 与 Mac 上 Pet Bridge 的链路: Wi-Fi STA + TCP 客户端 + 自动重连。
//
// 设备是 TCP 客户端, Mac 是服务端 —— 这样设备侧不需要接受入站连接, 也不依赖
// Bonjour/mDNS 发现。断开后按指数退避重连, 并把链路状态回调给应用层。
//
// 线程约定: 两个回调都在 bridge 任务(或 Wi-Fi 事件任务)上下文里执行。
// 回调里不要做阻塞操作, 也不要直接碰 LVGL —— 应用层负责把事件搬进队列。
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "pet_protocol.h"
#include "pet_settings.h"
#include "pet_state.h"

typedef struct {
    // 收到一条完整的协议消息。
    void (*on_message)(const pet_message_t *msg, void *user);
    // 链路状态变化: 只会是 PET_LINK_OFFLINE / PET_LINK_CONNECTING / PET_LINK_ONLINE。
    void (*on_link)(pet_link_state_t link, void *user);
    void *user;
} pet_bridge_sink_t;

// 初始化 NVS / netif / event loop 与 Wi-Fi STA, 并按 settings 配好凭据。
// 可重复调用; 已配置时会用新凭据重配。
esp_err_t pet_bridge_prepare(const pet_settings_t *settings);

// 启动后台连接任务。可重复调用(已经在跑时直接返回 ESP_OK)。
esp_err_t pet_bridge_start(const pet_bridge_sink_t *sink);

// 停止任务并断开连接。用于应用退出。可重复调用。
esp_err_t pet_bridge_stop(void);

// 向 Mac 发送一条消息(type 为协议类型名, text 可为 NULL)。
// 未连接时返回 ESP_ERR_INVALID_STATE。
esp_err_t pet_bridge_notify(const char *type, const char *text);

// 上报电池电量(0..100;-1 表示读不到, 会发 soc=null)。
// 除了立刻发一条, 还会记住这个值: 链路重连后紧跟 hello 补发一次, 否则菜单栏
// 要在下一次电量变化(最多 5 秒一拍)之前一直显示"未知"。
// 未连接时不算错误, 只是没人接收。
esp_err_t pet_bridge_notify_battery(int soc_percent);

// 当前是否已连接(供 UI 查询)。
bool pet_bridge_is_online(void);

// 取当前拿到的 IPv4 地址(点分十进制)。还没拿到 IP 时返回 false。
// 蓝牙配网用它判断"新凭据到底连上没有"。
bool pet_bridge_local_ip(char *out, size_t size);
