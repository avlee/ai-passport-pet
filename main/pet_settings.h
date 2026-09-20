// main/pet_settings.h
// Wi-Fi 与 Pet Bridge 连接参数的持久化(NVS)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define PET_SETTINGS_SSID_MAX 33
#define PET_SETTINGS_PASS_MAX 65
#define PET_SETTINGS_HOST_MAX 64
#define PET_SETTINGS_PIN_MAX 8

typedef struct {
    char     ssid[PET_SETTINGS_SSID_MAX];
    char     password[PET_SETTINGS_PASS_MAX];
    char     host[PET_SETTINGS_HOST_MAX];
    uint16_t port;
} pet_settings_t;

// 读取连接参数。只有在 NVS 里存在一整套显式覆盖时才使用 NVS 的值, 否则使用
// pet_config.h 的编译期默认值 —— 且不会把默认值写回 NVS, 避免改了 pet_config.h
// 重新烧录后旧的 SSID 仍然生效。
// 返回 ESP_OK 表示 out 已填好。
esp_err_t pet_settings_load(pet_settings_t *out);

// 覆盖写入连接参数。凭据只进 NVS, 不写日志。
esp_err_t pet_settings_save(const pet_settings_t *in);

// 清空连接参数, 下次启动重新使用编译期默认值。
// 配对码**不在**清空范围内 —— 它管的是"哪台机器能连上设备", 跟网络参数是两件事,
// 顺手换掉会逼用户再跑到设备跟前抄一遍码。
esp_err_t pet_settings_clear(void);

// 配对码。每次开配网页都会重新生成并覆盖存盘(换码逻辑在 pet_provision.c 的
// rotate_pin, 这里只管存取); pet_settings_clear() 不动它, 见上。
esp_err_t pet_settings_save_pin(const char *pin);

// 这组参数是否已经"配过网"。SSID 为空、或仍等于 pet_config.h 里的占位符
// (PET_WIFI_PLACEHOLDER_SSID)时为 false —— 设备据此决定开机要不要进蓝牙配网,
// 所以 pet_config_local.h 里填了真实 SSID 的老用法不受影响。
bool pet_settings_is_configured(const pet_settings_t *settings);
