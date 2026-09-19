// tests/host_stubs/esp_err.h
//
// 主机测试用的极简 esp_err.h 替身。
//
// 为什么需要它
// ------------
// main/pet_provision_parse.h 通过 pet_settings.h 间接包含 esp_err.h, 而这一层
// 是纯字符串逻辑 —— 它一个 ESP-IDF 函数都不调, 只是恰好和"设置"共用了一组长度
// 常量。为了能在 Mac 上直接编译它, 这里给一个只有类型的替身, 而不是把
// pet_settings.h 的接口改得不像项目里的写法。
//
// 注意: 这个文件只应被 tests/ 的编译命令看到(-Itests/host_stubs)。固件编译走
// ESP-IDF 自带的 esp_err.h, 绝不会走到这里 —— 否则真正的 ESP_OK/ESP_FAIL 就被
// 替成了假定义。不要把它加到 main/ 的 include 路径里。
#pragma once

#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK   0
#define ESP_FAIL (-1)

#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105

// 固件里这两个宏来自 esp_err.h, 我们这一层不用, 但保持定义以免以后扩展时踩空。
#define ESP_ERROR_CHECK(x) do { (void)(x); } while (0)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(x) (x)
