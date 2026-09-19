// main/pet_app.c
#include "pet_app.h"

#include "app_input.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "demo_menu.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "pet_atlas.h"
#include "pet_bridge.h"
#include "pet_config.h"
#include "pet_fonts.h"
#include "pet_protocol.h"
#include "pet_settings.h"
#include "pet_strings.h"
#include "pet_ui.h"

static const char *TAG = "pet_app";

static QueueHandle_t  s_input_queue;
static TaskHandle_t   s_input_task;
static TaskHandle_t   s_battery_task;
static pet_settings_t s_settings;
static bool           s_settings_ok;
static volatile bool  s_input_ready;
static int            s_last_soc = -1;

// ---------------------------------------------------------------------------
// 中文文案覆盖自检
// ---------------------------------------------------------------------------
// 基线 LVGL 只带 Montserrat, 没有任何汉字。字库是子集化的(GB2312), 所以新增文案
// 有可能落在子集之外, 表现是屏幕上出现方框。这里在开机时把界面上所有文案过一遍,
// 缺字直接报到串口 —— 比等到肉眼看屏幕快得多。
static void check_string_coverage(void)
{
    unsigned missing_count = 0;

    for (unsigned i = 0; i < PET_UI_STRINGS_COUNT; i++) {
        const pet_string_t *item = &PET_UI_STRINGS[i];
        const lv_font_t *font = item->px >= 20 ? pet_font_title() : pet_font_body();
        uint32_t missing = 0;
        if (!pet_font_covers_text(font, item->text, &missing)) {
            missing_count++;
            ESP_LOGW(TAG, "文案缺字: %s = \"%s\" 缺 U+%04X (%u px)", item->key,
                     item->text, (unsigned)missing, (unsigned)item->px);
        }
    }

    if (missing_count == 0) {
        ESP_LOGI(TAG, "中文文案覆盖自检通过(%u 条)", PET_UI_STRINGS_COUNT);
    } else {
        ESP_LOGE(TAG, "%u/%u 条文案缺字, 屏幕上会显示成方框; "
                      "请重新生成 assets/fonts(python3 tools/gen_pet_fonts.py)",
                 missing_count, PET_UI_STRINGS_COUNT);
    }
}

// ---------------------------------------------------------------------------
// Pet Bridge 事件(运行在 bridge 任务 / Wi-Fi 事件任务上)
// ---------------------------------------------------------------------------
static void on_bridge_link(pet_link_state_t link, void *user)
{
    (void)user;

    // 断线即「睡觉」: 压暗背光省电, 宠物本体也会被 pet_ui 压暗并放慢。
    bsp_display_backlight(link == PET_LINK_OFFLINE ? PET_SLEEP_BACKLIGHT_PCT
                                                  : PET_ACTIVE_BACKLIGHT_PCT);
    pet_ui_set_link(link);
}

static void on_bridge_message(const pet_message_t *msg, void *user)
{
    (void)user;

    switch (msg->type) {
    case PET_MSG_STATE:
        // 先更新文本再更新状态: 状态跳转可能触发过场动画, 文本先就位看起来更连贯。
        if (msg->has_text) pet_ui_set_text(msg->text);
        if (msg->has_state) pet_ui_set_codex(msg->state);
        break;

    case PET_MSG_TEXT:
        if (msg->has_text) pet_ui_set_text(msg->text);
        break;

    case PET_MSG_PING:
        (void)pet_bridge_notify("pong", NULL);
        break;

    case PET_MSG_NONE:
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// 电量
// ---------------------------------------------------------------------------
// CW2017 是 I2C 设备, 读一次要几毫秒。放在独立任务里, 避免拖住 LVGL 渲染任务。
// 初始化也在这里做: 首次初始化要写电池 profile 并等 SOC 收敛, 最坏 5 秒以上,
// 放在 pet_app_start() 里会把界面和 Bridge 一起拖住。
static bool battery_init(void)
{
    const unsigned attempts = 3;

    for (unsigned i = 1; i <= attempts; i++) {
        const esp_err_t err = bsp_battery_init();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "电量计就绪");
            return true;
        }
        ESP_LOGW(TAG, "CW2017 初始化失败 %u/%u: %s",
                 i, attempts, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // 没有电量计的板子照样能跑, 只是界面一直显示 "--", 所以只警告不失败。
    ESP_LOGW(TAG, "电量计不可用, 电量将一直显示 --; "
                  "用演示菜单的 I2C 扫描确认 0x63 是否在线");
    return false;
}

// 单次 I2C 读失败不值得把电量打回 "--"; 连续失败到一定次数才认定为真的掉了,
// 这时清空显示, 免得界面一直停在一个旧数值上骗人。
#define BATTERY_FAIL_TOLERANCE 6

static void battery_task(void *arg)
{
    (void)arg;
    (void)battery_init();

    unsigned failures = 0;

    for (;;) {
        const int soc = bsp_battery_soc();

        if (soc < 0) {
            if (s_last_soc >= 0 && ++failures >= BATTERY_FAIL_TOLERANCE) {
                ESP_LOGW(TAG, "连续 %u 次读电量失败, 改为显示未知", failures);
                s_last_soc = -1;
                pet_ui_set_battery(-1);
                failures = 0;
            }
        } else {
            failures = 0;
            if (soc != s_last_soc) {
                ESP_LOGI(TAG, "电量 %d%%", soc);
                s_last_soc = soc;
                pet_ui_set_battery(soc);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(PET_BATTERY_POLL_MS));
    }
}

// ---------------------------------------------------------------------------
// 按键
// ---------------------------------------------------------------------------
// 回调运行在共享 esp_timer 任务上, 只能入队。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!s_input_ready || s_input_queue == NULL) return;
    const app_input_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

// 进演示菜单再回来。演示菜单期间由它自己消费按键队列(同一个任务上下文),
// 所以这里直接同步调用、等它返回即可。
static void run_demo_menu(void)
{
    ESP_LOGI(TAG, "打开演示菜单");
    pet_ui_destroy();
    bsp_display_backlight(PET_ACTIVE_BACKLIGHT_PCT);

    demo_menu_run(s_input_queue);

    // pet_ui 的状态(链路/Codex/文案/电量)在 destroy 时被刻意保留了, 所以重建
    // 界面后立刻就是最新画面, 不会闪回旧状态。
    pet_ui_build();
    ESP_LOGI(TAG, "回到宠物界面");
}

static void handle_input(const app_input_t *in)
{
    if (in->event == BSP_BTN_LONG) {
        if (in->btn == BSP_BTN_UP) {
            run_demo_menu();
        } else if (in->btn == BSP_BTN_OK) {
            pet_ui_set_info_visible(true);
        }
        return;
    }

    if (in->event != BSP_BTN_CLICK) return;

    switch (in->btn) {
    case BSP_BTN_UP:
        // 互动动画是纯本机行为, 不需要联网。
        pet_ui_emote(PET_ANIM_WAVING);
        break;

    case BSP_BTN_DOWN:
        pet_ui_emote(PET_ANIM_JUMPING);
        break;

    case BSP_BTN_OK:
        if (pet_ui_info_visible()) {
            pet_ui_set_info_visible(false);
        } else {
            pet_ui_emote(PET_ANIM_JUMPING);
            // 顺手告诉 Codex 侧「宠物被戳了一下」。未连线时是空操作。
            (void)pet_bridge_notify("poke", NULL);
        }
        break;

    default:
        break;
    }
}

static void input_task(void *arg)
{
    (void)arg;
    app_input_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) != pdTRUE) continue;
        handle_input(&input);
    }
}

// ---------------------------------------------------------------------------
// 启动
// ---------------------------------------------------------------------------
esp_err_t pet_app_start(void)
{
    pet_atlas_init();
    pet_fonts_init();
    check_string_coverage();

    // 连接参数: 没有可用参数不算致命错误, 宠物照样能显示, 只是保持睡眠。
    if (pet_settings_load(&s_settings) == ESP_OK) {
        s_settings_ok = true;
        ESP_LOGI(TAG, "配对端 %s:%u, SSID \"%s\"", s_settings.host,
                 (unsigned)s_settings.port, s_settings.ssid);
    } else {
        ESP_LOGW(TAG, "没有可用的连接参数, 宠物将保持睡眠; "
                      "请填写 main/pet_config.h 后重新烧录");
    }

    pet_ui_init();
    pet_ui_build();
    if (!pet_ui_ready()) {
        ESP_LOGE(TAG, "宠物界面创建失败");
        return ESP_FAIL;
    }
    pet_ui_set_settings(s_settings_ok ? &s_settings : NULL);

    s_input_queue = xQueueCreate(APP_INPUT_QUEUE_DEPTH, sizeof(app_input_t));
    if (s_input_queue == NULL) return ESP_ERR_NO_MEM;
    s_input_ready = true;
    if (xTaskCreate(input_task, "pet_input", 4096, NULL, 5, &s_input_task) != pdPASS) {
        s_input_ready = false;
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = bsp_button_init(on_key, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(err));
    }

    // 4096 而非更小: 这个任务除了轮询还要跑 CW2017 初始化(profile 写入 + 日志)。
    if (xTaskCreate(battery_task, "pet_battery", 4096, NULL, 3,
                    &s_battery_task) != pdPASS) {
        ESP_LOGW(TAG, "电量任务创建失败, 电量将一直显示 --");
    }

    if (s_settings_ok) {
        err = pet_bridge_prepare(&s_settings);
        if (err == ESP_OK) {
            const pet_bridge_sink_t sink = {
                .on_message = on_bridge_message,
                .on_link = on_bridge_link,
                .user = NULL,
            };
            err = pet_bridge_start(&sink);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Pet Bridge 启动失败: %s; 宠物保持睡眠状态",
                     esp_err_to_name(err));
            bsp_display_backlight(PET_SLEEP_BACKLIGHT_PCT);
        }
    }

    ESP_LOGI(TAG, "就绪。上/下=互动, 确定=戳一下, 长按确定=信息, 长按上=演示菜单");
    return ESP_OK;
}
