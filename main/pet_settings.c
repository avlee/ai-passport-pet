// main/pet_settings.c
#include "pet_settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pet_config.h"

static const char *TAG = "pet_settings";

#define PET_NVS_NAMESPACE "petcfg"
#define KEY_SSID "ssid"
#define KEY_PASS "pass"
#define KEY_HOST "host"
#define KEY_PORT "port"
#define KEY_PIN "pin"

// 从 NVS 读出一个有界字符串。键不存在或类型不符时返回 false。
static bool read_string(nvs_handle_t handle, const char *key, char *out, size_t size)
{
    size_t length = size;
    if (nvs_get_str(handle, key, out, &length) != ESP_OK) return false;
    out[size - 1] = '\0';
    return true;
}

static void apply_defaults(pet_settings_t *settings)
{
    memset(settings, 0, sizeof(*settings));
    strlcpy(settings->ssid, PET_WIFI_SSID, sizeof(settings->ssid));
    strlcpy(settings->password, PET_WIFI_PASSWORD, sizeof(settings->password));
    strlcpy(settings->host, PET_BRIDGE_HOST, sizeof(settings->host));
    settings->port = (uint16_t)PET_BRIDGE_PORT;
}

// NVS 是这里的第一个使用者, 不能假设别处已经初始化过 —— 否则 nvs_open 只会
// 返回 ESP_ERR_NVS_NOT_INITIALIZED, NVS 覆盖路径永远读不到, 看起来又像"配置没生效"。
//
// 分区损坏(无空闲页 / 版本不符)时不擦除: 擦掉的是用户数据, 而编译期默认值本来
// 就能兜住, 没必要为此冒险。这里只报错并让调用方退回默认值。
static bool ensure_nvs(void)
{
    const esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 初始化失败(%s), 无法读取 NVS 覆盖配置",
                 esp_err_to_name(err));
        return false;
    }
    return true;
}

esp_err_t pet_settings_load(pet_settings_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    if (!ensure_nvs()) {
        apply_defaults(out);
        return ESP_OK;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(PET_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 打开失败(%s), 本次使用编译期默认值", esp_err_to_name(err));
        apply_defaults(out);
        return ESP_OK;
    }

    pet_settings_t stored;
    apply_defaults(&stored);

    // 只有在 NVS 里存在「一整套显式覆盖」时才采用它; 否则一律用 pet_config.h
    // 的编译期默认值, 并且不写回 NVS。
    //
    // 这样做的原因: 如果开机就把默认值落盘, 之后开发者改了 pet_config.h 再烧录,
    // 旧的 SSID 会继续生效, 变成一个很难发现的坑。
    bool complete = read_string(handle, KEY_SSID, stored.ssid, sizeof(stored.ssid));
    complete = read_string(handle, KEY_PASS, stored.password,
                           sizeof(stored.password)) && complete;
    complete = read_string(handle, KEY_HOST, stored.host, sizeof(stored.host)) &&
               complete;

    uint16_t port = 0;
    const esp_err_t port_err = nvs_get_u16(handle, KEY_PORT, &port);
    if (port_err == ESP_OK && port != 0) {
        stored.port = port;
    } else {
        complete = false;
    }

    nvs_close(handle);

    if (!complete) {
        ESP_LOGI(TAG, "NVS 无完整覆盖配置, 使用 pet_config.h 的编译期默认值");
        apply_defaults(out);
    } else {
        *out = stored;
    }

    // 密码不写日志, 避免凭据进入串口输出。
    ESP_LOGI(TAG, "Wi-Fi SSID=\"%s\", Bridge=%s:%u (来源: %s)", out->ssid,
             out->host, (unsigned)out->port, complete ? "NVS" : "pet_config.h");
    return ESP_OK;
}

esp_err_t pet_settings_save(const pet_settings_t *in)
{
    if (in == NULL) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(PET_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, KEY_SSID, in->ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_PASS, in->password);
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_HOST, in->host);
    if (err == ESP_OK) err = nvs_set_u16(handle, KEY_PORT, in->port);
    if (err == ESP_OK) err = nvs_commit(handle);

    nvs_close(handle);
    return err;
}

esp_err_t pet_settings_clear(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(PET_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    // 逐键删除而不是 nvs_erase_all(): 配对码也要留在这一个命名空间里, 而"忘了
    // 网络参数"不该顺手把配对码也换掉。
    //
    // 键不存在不算失败 —— 用户可能只配了一半就想清掉。
    const char *keys[] = { KEY_SSID, KEY_PASS, KEY_HOST, KEY_PORT };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const esp_err_t erase = nvs_erase_key(handle, keys[i]);
        if (erase != ESP_OK && erase != ESP_ERR_NVS_NOT_FOUND) {
            nvs_close(handle);
            return erase;
        }
    }
    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

bool pet_settings_load_pin(char *out, size_t size)
{
    if (out == NULL || size == 0) return false;
    out[0] = '\0';

    if (!ensure_nvs()) return false;

    nvs_handle_t handle;
    if (nvs_open(PET_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;

    const bool ok = read_string(handle, KEY_PIN, out, size);
    nvs_close(handle);
    return ok && out[0] != '\0';
}

esp_err_t pet_settings_save_pin(const char *pin)
{
    if (pin == NULL || pin[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (!ensure_nvs()) return ESP_ERR_NVS_NOT_INITIALIZED;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(PET_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, KEY_PIN, pin);
    if (err == ESP_OK) err = nvs_commit(handle);

    nvs_close(handle);
    return err;
}

bool pet_settings_is_configured(const pet_settings_t *settings)
{
    if (settings == NULL || settings->ssid[0] == '\0') return false;
    return strcmp(settings->ssid, PET_WIFI_PLACEHOLDER_SSID) != 0;
}
