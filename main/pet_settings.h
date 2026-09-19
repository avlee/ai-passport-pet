// main/pet_settings.h
// Wi-Fi 与 Pet Bridge 连接参数的持久化(NVS)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define PET_SETTINGS_SSID_MAX 33
#define PET_SETTINGS_PASS_MAX 65
#define PET_SETTINGS_HOST_MAX 64

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

// 清空存档, 下次启动重新使用编译期默认值。
esp_err_t pet_settings_clear(void);
