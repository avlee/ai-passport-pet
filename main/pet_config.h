// main/pet_config.h
// Codex 宠物应用的可配置项。
//
// 第一次开机(或清空 NVS 后)会把这些默认值写进 NVS, 之后一律以 NVS 为准,
// 所以改完这里需要重新烧录, 或者先清空 NVS。
//
// 不想把网络凭据写进本文件? 在 main/ 下新建 pet_config_local.h 覆盖下面的宏,
// 该文件名已加入 .gitignore, 不会被提交。
#if defined(__has_include)
#if __has_include("pet_config_local.h")
#include "pet_config_local.h"
#endif
#endif

// 设备只支持 2.4 GHz Wi-Fi —— ESP32-C3 没有 5 GHz 射频。
#ifndef PET_WIFI_SSID
#define PET_WIFI_SSID "your-2g-ssid"
#endif

#ifndef PET_WIFI_PASSWORD
#define PET_WIFI_PASSWORD "your-password"
#endif

// Mac 上 Pet Bridge 的地址: 局域网 IP 或可解析的主机名。
// 在 Mac 上用 `ipconfig getifaddr en0` 查看自己的局域网 IP。
#ifndef PET_BRIDGE_HOST
#define PET_BRIDGE_HOST "192.168.1.10"
#endif

#ifndef PET_BRIDGE_PORT
#define PET_BRIDGE_PORT 8765
#endif

// 通过 hello 消息上报给 Bridge, 便于在 Mac 侧确认设备身份。
#define PET_FIRMWARE_VERSION "0.1.0"
#define PET_PACKAGE_ID "sophie-portrait"

// TCP 重连退避(毫秒): 从最小值开始倍增, 上限为最大值。
#define PET_BRIDGE_RETRY_MIN_MS 1000
#define PET_BRIDGE_RETRY_MAX_MS 15000

// 超过该时间没有收到任何消息(含 ping)就认为链路已经掉线。
#define PET_BRIDGE_IDLE_TIMEOUT_MS 12000

// 睡眠(断线)时把背光降到该百分比, 省电同时保留可读性。
#define PET_SLEEP_BACKLIGHT_PCT 45
#define PET_ACTIVE_BACKLIGHT_PCT 100

// 电量轮询间隔。CW2017 走 I2C, 放在独立任务里读, 不要占用 LVGL 渲染任务。
#define PET_BATTERY_POLL_MS 5000
