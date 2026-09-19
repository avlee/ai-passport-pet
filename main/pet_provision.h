// main/pet_provision.h
// 蓝牙配网: 让 Mac 侧的菜单栏应用把 Wi-Fi 凭据与配对端地址发给设备。
//
// 为什么需要它
// ------------
// 把 SSID/密码编进 pet_config_local.h 再重新烧录, 换个网络就得重编一次, 而且
// 凭据会落到开发机上。改成 BLE 配网之后: 设备开机广播, Mac 侧输入一次, 参数存进
// NVS, 之后换网络只要长按设备上的按键重开配网窗口即可。
//
// 配对码
// ------
// 设备屏幕上显示一个 4 位数字, Mac 侧必须输入正确才能下发凭据。配对码在每次连接
// 建立时重新生成, 连错 PET_PROVISION_MAX_ATTEMPTS 次就断开并换码。
//
// 线程约定
// --------
// - on_state 在 NimBLE 宿主任务或配网任务上下文里执行: 不要阻塞, 不要碰 LVGL。
// - apply 在配网任务上下文里执行, **可以阻塞**(最多 PET_PROVISION_APPLY_TIMEOUT_MS),
//   因为它要等 Wi-Fi 真的连上。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "pet_settings.h"

typedef enum {
    PET_PROV_OFF = 0,      // 窗口没开
    PET_PROV_ADVERTISING,  // 广播中, 等 Mac 连接
    PET_PROV_CONNECTED,    // 已连接, 等配对码
    PET_PROV_PAIRED,       // 配对码通过, 等 Wi-Fi 参数
    PET_PROV_APPLYING,     // 参数收齐了, 正在保存并联网
    PET_PROV_DONE,         // 配网成功
    PET_PROV_FAILED,       // 这次失败(窗口仍然开着, 可以重来)
} pet_provision_state_t;

// 应用层实现: 用新参数把 Wi-Fi 与 Bridge 拉起来, 成功时把拿到的 IPv4 填进 ip_out。
// 返回 ESP_OK 才算这次配网成功。
typedef esp_err_t (*pet_provision_apply_t)(const pet_settings_t *settings,
                                           char *ip_out, size_t ip_out_size);

typedef struct {
    // 状态变化。detail 是给界面看的简短说明: DONE 时是 IP, FAILED 时是原因,
    // 其余状态为 NULL。
    void (*on_state)(pet_provision_state_t state, const char *detail, void *user);
    pet_provision_apply_t apply;
    void *user;
} pet_provision_sink_t;

// 打开配网窗口。current 是当前生效的参数(用来填 info 特征与草稿的初值)。
// 已经在跑时按新 sink 复用, 直接返回 ESP_OK。
esp_err_t pet_provision_start(const pet_provision_sink_t *sink,
                              const pet_settings_t *current);

// 关闭窗口并释放 NimBLE。可重复调用。
esp_err_t pet_provision_stop(void);

// 窗口是否开着(供 UI 与应用层判断, 也用来避开演示菜单里的 BLE 示例)。
bool pet_provision_active(void);

// 当前配对码(4 位数字)。窗口没开或还没生成时返回 false。
// 用拷贝而不是返回指针: 配对码虽然长期不变, 但会在输错用尽次数时换掉, 界面拿着
// 一个会变的指针迟早会读到半截。
bool pet_provision_pin(char *out, size_t size);

// 对外广播的设备名, 形如 CodexPet-75B4(取蓝牙 MAC 后两字节)。只在窗口打开时
// 写入, 之后不再变。
const char *pet_provision_device_name(void);
