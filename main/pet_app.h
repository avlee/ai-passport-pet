// main/pet_app.h
// Codex 宠物应用: 把 BSP、宠物界面、按键和 Pet Bridge 串起来。
#pragma once

#include "esp_err.h"

// 启动宠物应用。要求 bsp_display_init() 与 bsp_lvgl_init() 已经成功。
//
// 失败时返回错误码, 但已经建立起来的部分(例如界面)会保留, 便于在串口上看出
// 到底哪一环没起来; 不会中途 panic。
esp_err_t pet_app_start(void);
