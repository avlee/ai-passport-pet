// main/pet_config.h
// Codex 宠物应用的可配置项。
//
// 配置来源优先级:
//   1) main/pet_config_local.h —— 本机覆盖, 已加入 .gitignore, 适合放网络凭据;
//   2) 本文件下面的编译期默认值。
// 仅当 NVS 里已存在一整套显式覆盖(由 pet_settings_save 写入)时才以 NVS 为准;
// 本文件不会在开机时把默认值写回 NVS, 所以改完这里重新烧录即刻生效, 不必清 NVS。
//
// 不想把网络凭据写进本文件? 在 main/ 下新建 pet_config_local.h 覆盖下面的宏,
// 该文件名已加入 .gitignore, 不会被提交。
#if defined(__has_include)
#if __has_include("pet_config_local.h")
#include "pet_config_local.h"
#endif
#endif

// 编译期默认 SSID 的占位符。设备用它判断"这台机器还没配过网": 只要实际生效的
// SSID 仍等于这个占位符, 开机就进入蓝牙配网等 Mac 下发凭据; 一旦在
// pet_config_local.h 里填了真实 SSID, 就照旧直接联网, 行为与从前一致。
#define PET_WIFI_PLACEHOLDER_SSID "your-2g-ssid"

// 设备只支持 2.4 GHz Wi-Fi —— ESP32-C3 没有 5 GHz 射频。
#ifndef PET_WIFI_SSID
#define PET_WIFI_SSID PET_WIFI_PLACEHOLDER_SSID
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

// 这里以前有一个 PET_PACKAGE_ID("sophie-portrait"), 现在**故意没有了**。
// 宠物不再是编译期常量: 它躺在独立的 pets 分区里, 由 Pet Bridge 从 Mac 侧推下来。
// 设备上报的宠物身份一律现读槽里那一份(pet_slot_pet_id()), 槽空则报 "none" ——
// 菜单栏看到 none 就把默认那只推下来。留着编译期默认值只会造出第二个真相,
// 而且在换过宠物之后它必然是错的。

// TCP 重连退避(毫秒): 从最小值开始倍增, 上限为最大值。
#define PET_BRIDGE_RETRY_MIN_MS 1000
#define PET_BRIDGE_RETRY_MAX_MS 15000

// 超过该时间没有收到任何消息(含 ping)就认为链路已经掉线。
#define PET_BRIDGE_IDLE_TIMEOUT_MS 12000

// 睡眠(断线)时把背光降到该百分比, 省电同时保留可读性。
#define PET_SLEEP_BACKLIGHT_PCT 45
#define PET_ACTIVE_BACKLIGHT_PCT 100

// 空闲多久自动息屏(毫秒)。0 表示不自动息屏。
//
// 只统计"真的产生了新内容"的事件: Bridge 下发的状态/文本、按键、链路恢复。
// **Bridge 每 5 秒一次的 ping 刻意不算**(节拍见 tools/pet_bridge.py 的
// PING_INTERVAL_S) —— 算的话屏幕永远关不掉, 这个功能等于没做。
// 配网页与宠物接收页期间不自动息屏: 那两块画面用户必须看得见, 见 pet_app.c。
#define PET_SCREEN_IDLE_MS 60000

// 电量轮询间隔。CW2017 走 I2C, 放在独立任务里读, 不要占用 LVGL 渲染任务。
#define PET_BATTERY_POLL_MS 5000

// --- 蓝牙配网 ---
// 配网窗口最长开多久(毫秒)。到点自动关掉广播回到睡眠态, 免得设备一直可被扫到;
// 长按下键可以重新打开。
#define PET_PROVISION_WINDOW_MS 180000

// 配对码连续错几次就断开这次连接, 并换一个新配对码重来。
// 注意配对码本身是长期存 NVS 的(见 pet_settings_save_pin), 不随连接或开窗变动;
// 只有用尽这个次数才会换 —— 所以这个上限是"稳定的码"得以安全的前提, 别调大。
#define PET_PROVISION_MAX_ATTEMPTS 3

// 收到 Wi-Fi 参数后最多等多久拿到 IP(毫秒)。超过就判定这次配网失败,
// 但参数已经存进 NVS 了, 下次开机还会照它重试。
#define PET_PROVISION_APPLY_TIMEOUT_MS 25000
