// main/pet_app.c
#include "pet_app.h"

#include <string.h>

#include "app_input.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "demo_menu.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "pet_atlas.h"
#include "pet_bridge.h"
#include "pet_config.h"
#include "pet_fonts.h"
#include "pet_protocol.h"
#include "pet_provision.h"
#include "pet_screen.h"
#include "pet_settings.h"
#include "pet_slot.h"
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

// 屏幕电源。开机先按"还没链路"处理: pet_ui 的初值也是 OFFLINE(宠物一开始就在
// 睡觉), 两边一致, 免得开机的第一帧亮着而宠物是睡着的。
static pet_screen_t       s_screen;
static esp_timer_handle_t s_screen_timer;
static bool               s_link_offline = true;
static bool               s_anim_on = true;    // 上次同步给 pet_ui 的渲染开关
static int                s_backlight_pct = -1;  // 上次下发的背光, -1 = 还没写过
static bool               s_screen_was_on = true;  // 上次 apply_screen 时的亮灭

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
// 屏幕电源(空闲息屏 / 有消息亮屏)
// ---------------------------------------------------------------------------
// 判定本身在 pet_screen.c(纯逻辑, 主机测试逐条钉住); 这里只做两件机器相关的事:
// 把背光写下去, 以及息屏时停掉逐帧重绘。
//
// 背光**只有这一个写入口**: 从前 pet_app 在链路回调里写一次、启动失败时又写一次,
// 再叠上演示菜单自己那一摊, 排查"屏幕怎么还亮着"得同时看三处, 而且谁都可能把别人
// 的值覆盖掉。
static void apply_screen(void)
{
    const bool on = pet_screen_is_on(&s_screen);

    // 联动 bridge 的重连节奏(见 pet_bridge.h 的 set_doze/wake)。doze 是纯条件
    // (息屏 && 链路不通), 每次都同步。**必须用 pet_bridge_is_online() 而不是
    // s_link_offline**: 重试期间 bridge 任务每轮都报 CONNECTING, on_bridge_link
    // 会把 s_link_offline 置回 false —— 而息屏恰恰发生在重试等待期间, 用它判定
    // 的话 doze 永远设不上(真机踩过: 退避封顶卡死在 15s, 日志静默)。
    // wake 才需要翻转沿: 只在从灭到亮且链路不通时踢一次。
    const bool link_down = !pet_bridge_is_online();
    pet_bridge_set_doze(!on && link_down);
    if (on && !s_screen_was_on && link_down) pet_bridge_wake();
    s_screen_was_on = on;

    const uint8_t percent = pet_screen_backlight(&s_screen, s_link_offline);

    if ((int)percent != s_backlight_pct) {
        s_backlight_pct = (int)percent;
        bsp_display_backlight(percent);
        if (on) {
            ESP_LOGI(TAG, "亮屏, 背光 %u%%", (unsigned)percent);
        } else {
            const int64_t idle_s =
                (esp_timer_get_time() - s_screen.last_activity_us) / 1000000;
            ESP_LOGI(TAG, "息屏, 背光 0%% (已空闲 %lld 秒)",
                     (long long)idle_s);
        }
    }

    if (on != s_anim_on) {
        s_anim_on = on;
        // 息屏时没必要逐帧重绘: 画面看不见, 而每帧都要把精灵那 129x198x2 字节经
        // SPI 推给面板。亮屏后从当前帧接着播, 不跳帧。
        pet_ui_set_anim_enabled(on);
    }
}

// 有新内容: 亮屏并重新计时。消息、按键、链路恢复都走这里。
static void screen_touch(void)
{
    pet_screen_touch(&s_screen, esp_timer_get_time());
    apply_screen();
}

// 配网页 / 宠物接收页 / 演示菜单期间不许息屏: 那几块画面用户必须看得见。
static void screen_hold(bool hold)
{
    pet_screen_set_hold(&s_screen, hold, esp_timer_get_time());
    apply_screen();
}

// 用户锁定(双击确定息屏)。锁定后消息再吵也不亮屏, 任意键解锁。
static void screen_set_user_lock(bool lock)
{
    pet_screen_set_user_lock(&s_screen, lock, esp_timer_get_time());
    apply_screen();
}

// 一秒一拍。息屏判定的精度远用不到更细, 而每秒一次唤醒的代价可以忽略。
static void screen_tick(void *arg)
{
    (void)arg;
    if (pet_screen_update(&s_screen, esp_timer_get_time(),
                          (int64_t)PET_SCREEN_IDLE_MS * 1000)) {
        apply_screen();
    }
}

// ---------------------------------------------------------------------------
// Pet Bridge 事件(运行在 bridge 任务 / Wi-Fi 事件任务上)
// ---------------------------------------------------------------------------
static void on_bridge_link(pet_link_state_t link, void *user)
{
    (void)user;

    const bool was_offline = s_link_offline;
    s_link_offline = (link == PET_LINK_OFFLINE);

    // 链路**恢复**算"有新消息", 亮屏; 掉线不算 —— 宠物这时本来就进入睡眠(压暗 +
    // 放慢), 屏幕跟着暗下来才一致; 何况桥接重连时每一轮都会报一次 CONNECTING,
    // 把它当消息的话, 断网期间屏幕反而一直被点亮。
    if (was_offline && !s_link_offline) {
        screen_touch();
    } else {
        apply_screen();
    }

    pet_ui_set_link(link);
}

static void on_bridge_message(const pet_message_t *msg, void *user)
{
    (void)user;

    // ping 是链路保活(每 5 秒一条), 不是"新内容" —— 让它点亮屏幕的话, 息屏永远
    // 不会发生。其余消息都意味着 Codex 那边有动静: 先亮屏, 再更新画面。
    if (msg->type != PET_MSG_PING) screen_touch();

    switch (msg->type) {
    case PET_MSG_STATE:
        // 先更新文本再更新状态: 状态跳转可能触发过场动画, 文本先就位看起来更连贯。
        if (msg->has_text) pet_ui_set_text(msg->text);
        if (msg->has_state) pet_ui_set_codex(msg->state);
        break;

    case PET_MSG_TEXT:
        if (msg->has_text) pet_ui_set_text(msg->text);
        break;

    case PET_MSG_LIMITS:
        // 限额变化意味着 Codex 那边真的发了请求(或窗口翻页), 让它算"新内容":
        // 屏幕在息屏时被点亮一次, 用户能看到能量槽跳一下。窗口没提供的传 -1,
        // 由界面隐藏对应的槽。
        pet_ui_set_limits(msg->limits_primary_used, msg->limits_weekly_used);
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
// 宠物包接收
// ---------------------------------------------------------------------------
// 这一段跑在 bridge 任务上, 而且是在**收字节的过程中**被反复调用, 所以每个回调都
// 必须短: 真正耗时的擦写在 pet_slot 里(它自己按 16 KB 一段让出 CPU), 这里只更新
// 屏幕上的进度。
//
// 一次传输的完整顺序(不能颠倒):
//   on_pet_begin   拆界面(引用着 mmap 的图像描述符) -> 解除映射 -> 擦掉槽头部
//   on_pet_data    边收边写, 顺便报进度
//   on_pet_end     核对长度与 CRC -> 重新映射 -> 重建界面
//
// 中途掉线会走 on_pet_end(false): 槽的头部在 begin 时就擦掉了, 所以这个时候
// 槽里既没有旧宠物也没有新宠物 —— 界面必须回到"还没有宠物", 不能停在传输页上。
static uint32_t s_pet_size;
static uint32_t s_pet_crc;
static bool     s_pet_rx_failed;

static esp_err_t on_pet_begin(const char *pet_id, uint32_t size, uint32_t crc32,
                              void *user)
{
    (void)user;

    // 配网时蓝牙和 Wi-Fi 同时在跑; 再压一次几十秒的 flash 擦写会让两边都超时。
    // 菜单栏收到失败回执后会隔一会儿重试, 所以这里拒收是安全的。
    if (pet_provision_active()) {
        ESP_LOGW(TAG, "配网进行中, 拒绝接收宠物包");
        return ESP_ERR_INVALID_STATE;
    }
    if (pet_slot_writing()) {
        ESP_LOGW(TAG, "上一份宠物包还在写, 拒绝接收新的");
        return ESP_ERR_INVALID_STATE;
    }

    // 进度必须看得见: 屏幕上正在走进度条, 这一刻息屏只会让人以为设备死了。
    screen_hold(true);

    // 先拆界面再解除映射: 顺序反了, 界面上的图像描述符就指向一块已经归还的
    // MMU 窗口。pet_ui_begin_transfer() 会把宠物界面整块删掉并顶上一块不引用
    // 图集的进度屏 —— 传输期间屏幕不会黑。
    pet_ui_begin_transfer(pet_id, size);
    pet_atlas_unload();

    s_pet_size = size;
    s_pet_crc = crc32;
    s_pet_rx_failed = false;

    const esp_err_t err = pet_slot_begin(size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "无法开始接收宠物包: %s", pet_slot_last_error());
        s_pet_rx_failed = true;
        pet_ui_set_transfer_message(PET_STR_TRANSFER_FAILED, PET_TONE_BAD);
        return err;
    }
    return ESP_OK;
}

static void on_pet_data(const uint8_t *data, size_t len, void *user)
{
    (void)user;
    // 已经失败就别再写了 —— slot 会把后续每一段都拒掉, 只会把日志刷满。
    if (s_pet_rx_failed) return;

    if (pet_slot_write(data, len) != ESP_OK) {
        ESP_LOGE(TAG, "写宠物包失败: %s", pet_slot_last_error());
        pet_slot_abort();
        s_pet_rx_failed = true;
        pet_ui_set_transfer_message(PET_STR_TRANSFER_FAILED, PET_TONE_BAD);
        return;
    }
    pet_ui_set_transfer_progress(pet_slot_written());
}

static void on_pet_end(bool ok, void *user)
{
    (void)user;

    bool installed = false;

    if (ok && !s_pet_rx_failed) {
        // finish() 自己核对长度与整包 CRC32(它是边收边算的, 不用把 3 MB 从 flash
        // 里读回来)。
        const esp_err_t err = pet_slot_finish(s_pet_size, s_pet_crc);
        if (err == ESP_OK && pet_atlas_load()) {
            installed = true;
        } else {
            ESP_LOGE(TAG, "宠物包校验失败: %s", pet_slot_last_error());
        }
    } else {
        pet_slot_abort();
    }

    if (installed) {
        ESP_LOGI(TAG, "宠物已换成 %s", pet_atlas_pet_id());
        pet_ui_end_transfer();
        pet_ui_build();   // 新宠物的舞台尺寸可能不同, 界面必须按它重建
    } else {
        // 失败要在屏幕上留一会儿: 立刻收回的话用户只会看到界面闪一下, 根本不知道
        // 刚才发生了什么。停顿期间链路是空闲的, 压住 bridge 任务两秒没有代价。
        pet_ui_set_transfer_message(PET_STR_TRANSFER_FAILED "\n" PET_STR_TRANSFER_AGAIN,
                                    PET_TONE_BAD);
        vTaskDelay(pdMS_TO_TICKS(2000));

        // 槽这时是空的(头部已在 begin 时擦掉), 所以底下是"还没有宠物"的画面;
        // 但也要把槽重新挂一遍: 万一失败发生在 finish 之后, 槽里其实是有东西的。
        pet_ui_end_transfer();
        (void)pet_atlas_load();
        pet_ui_build();
    }

    s_pet_rx_failed = false;
    (void)pet_bridge_notify_pet_done(installed);
}

// ---------------------------------------------------------------------------
// 蓝牙配网
// ---------------------------------------------------------------------------
// 这台设备原先靠"改 pet_config_local.h 再重新烧录"换网络, 换个 Wi-Fi 就得重编一次。
// 现在改成: 开机没有可用凭据就进配网, 或者长按下键随时重开, Mac 侧的菜单栏应用
// 输入配对码后把 SSID/密码/配对端地址一起发过来, 存进 NVS。

// 把当前 s_settings 拉起来。开机走一次; 配网成功后要再走一次(参数已经换成 Mac
// 下发的了)。sink 放在文件作用域, 因为 pet_bridge 会记住它, 由 bridge 任务回调。
static const pet_bridge_sink_t s_bridge_sink = {
    .on_message = on_bridge_message,
    .on_link = on_bridge_link,
    .on_pet_begin = on_pet_begin,
    .on_pet_data = on_pet_data,
    .on_pet_end = on_pet_end,
    .user = NULL,
};

static esp_err_t start_bridge(void)
{
    esp_err_t err = pet_bridge_prepare(&s_settings);
    if (err == ESP_OK) err = pet_bridge_start(&s_bridge_sink);
    return err;
}

// 设备只上报简短的原因码, 中文提示在这里落地 —— 文案的单一来源仍是
// pet_strings.h, 字库覆盖由 tests/test_pet_font_coverage.py 保证。
static const char *provision_error_text(const char *detail)
{
    if (detail != NULL) {
        if (strcmp(detail, "save failed") == 0) return PET_STR_PROV_ERR_SAVE;
        if (strcmp(detail, "connect failed") == 0) return PET_STR_PROV_ERR_CONNECT;
    }
    return PET_STR_PROV_FAILED;
}

// "保存参数 + 联网"会阻塞(等 DHCP 最坏几十秒), 由 pet_provision 放在它自己的任务
// 里调用, 所以不会压在蓝牙协议栈上。
static esp_err_t apply_provisioned(const pet_settings_t *settings, char *ip_out,
                                   size_t ip_out_size)
{
    // 新旧参数可能完全不是同一个网络, 先把旧链路彻底收掉再重建, 免得重连退避还
    // 挂着上一个网络的节奏。
    (void)pet_bridge_stop();

    s_settings = *settings;
    s_settings_ok = true;

    const esp_err_t err = start_bridge();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "按新参数启动 Bridge 失败: %s", esp_err_to_name(err));
        return err;
    }

    pet_ui_set_settings(&s_settings);

    const int64_t deadline =
        esp_timer_get_time() + (int64_t)PET_PROVISION_APPLY_TIMEOUT_MS * 1000;
    while (esp_timer_get_time() < deadline) {
        if (pet_bridge_local_ip(ip_out, ip_out_size)) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ESP_LOGW(TAG, "等了 %d 秒仍没拿到 IP, 这次配网判定失败",
             PET_PROVISION_APPLY_TIMEOUT_MS / 1000);
    return ESP_ERR_TIMEOUT;
}

// 回调可能来自 NimBLE 宿主任务或配网任务。这里只改文案 + 通知界面: pet_ui 的每个
// 入口都自己加 LVGL 锁, 因此从这两个线程调用是安全的。
static void on_provision_state(pet_provision_state_t state, const char *detail,
                               void *user)
{
    (void)user;

    if (state == PET_PROV_OFF) {
        // 配网结束(成功、超时、或被演示菜单接走蓝牙)都要收掉这一层, 否则宠物
        // 界面会一直被它盖着。留条日志, 免得"到底收没收"只能靠盯屏幕。
        ESP_LOGI(TAG, "配网界面: 收起");
        screen_hold(false);
        pet_ui_set_provision_visible(false);
        return;
    }

    char pin[8] = { 0 };
    const bool has_pin = pet_provision_pin(pin, sizeof(pin));

    char text[96];
    pet_tone_t tone = PET_TONE_INFO;

    switch (state) {
    case PET_PROV_ADVERTISING:
        snprintf(text, sizeof(text), "%s", PET_STR_PROV_WAITING);
        break;
    case PET_PROV_CONNECTED:
        snprintf(text, sizeof(text), "%s", PET_STR_PROV_CONNECTED);
        break;
    case PET_PROV_PAIRED:
        snprintf(text, sizeof(text), "%s", PET_STR_PROV_PAIRED);
        tone = PET_TONE_GOOD;
        break;
    case PET_PROV_APPLYING:
        snprintf(text, sizeof(text), "%s", PET_STR_PROV_APPLYING);
        break;
    case PET_PROV_DONE:
        // 带上拿到的地址: 一眼就能看出设备落在哪个网段, 排查时很省事。
        snprintf(text, sizeof(text), "%s %s", PET_STR_PROV_DONE,
                 detail != NULL ? detail : "");
        tone = PET_TONE_GOOD;
        break;
    case PET_PROV_FAILED:
        snprintf(text, sizeof(text), "%s", provision_error_text(detail));
        tone = PET_TONE_BAD;
        break;
    case PET_PROV_OFF:
    default:
        snprintf(text, sizeof(text), "%s", PET_STR_PROV_WAITING);
        break;
    }

    ESP_LOGI(TAG, "配网界面: %s", text);
    pet_ui_set_provision_visible(true);
    pet_ui_set_provision(pet_provision_device_name(), has_pin ? pin : NULL, text,
                         tone);
}

static void open_provision_window(void)
{
    if (pet_provision_active()) {
        ESP_LOGI(TAG, "配网窗口已经开着");
        return;
    }

    const pet_provision_sink_t sink = {
        .on_state = on_provision_state,
        .apply = apply_provisioned,
        .user = NULL,
    };

    const esp_err_t err =
        pet_provision_start(&sink, s_settings_ok ? &s_settings : NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配网窗口打开失败: %s", esp_err_to_name(err));
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
                (void)pet_bridge_notify_battery(-1);   // 菜单栏跟着回到"未知"
                failures = 0;
            }
        } else {
            failures = 0;
            if (soc != s_last_soc) {
                ESP_LOGI(TAG, "电量 %d%%", soc);
                s_last_soc = soc;
                pet_ui_set_battery(soc);
                // 顺手报给 Mac: 菜单栏也显示电量。未连线时是空操作。
                (void)pet_bridge_notify_battery(soc);
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

    // 演示菜单里的 BLE 示例和配网共用同一个 NimBLE 栈, 不能同时占着 —— 先把配网
    // 窗口关掉, 退出后再按需重开。
    const bool had_provision = pet_provision_active();
    if (had_provision) {
        ESP_LOGI(TAG, "先关掉配网窗口, 把蓝牙让给演示菜单");
        (void)pet_provision_stop();
    }

    pet_ui_destroy();
    // 演示菜单里有人直接改背光(亮度示例页会停在任意档位), 所以让息屏策略先让开。
    // 亮度本身不用在这里给: hold 期间一律全亮, apply_screen() 已经写到 100 了。
    screen_hold(true);

    demo_menu_run(s_input_queue);

    // pet_ui 的状态(链路/Codex/文案/电量)在 destroy 时被刻意保留了, 所以重建
    // 界面后立刻就是最新画面, 不会闪回旧状态。
    pet_ui_build();
    // 演示菜单期间有人直接写过背光(亮度页会停在任意档位), 缓存里的值已经不作数:
    // 不清掉的话, 回到宠物界面时会因为"算出来和缓存一样"而漏掉这次写。
    s_backlight_pct = -1;
    screen_hold(false);
    if (had_provision) {
        ESP_LOGI(TAG, "演示菜单退出, 重新打开配网窗口");
        open_provision_window();
    }
    ESP_LOGI(TAG, "回到宠物界面");
}

static void handle_input(const app_input_t *in)
{
    // 锁定期(双击确定息屏)任何按键先解锁亮屏。动作照常执行 —— 和下面"任何按键
    // 都先亮屏"是同一条约定: 手里先有的是手指, 这一下必须立刻看到反应。
    if (pet_screen_is_locked(&s_screen)) {
        ESP_LOGI(TAG, "按键解锁");
        screen_set_user_lock(false);
    }

    // 任何按键都先亮屏并重置空闲计时: 屏幕黑着的时候, 用户手里先有的是手指, 这一下
    // 必须立刻看到反应。按键动作本身照常执行 —— 上/下/确定都是本机的小动画或面板
    // 开关, 顺手做掉比"先按一下唤醒、动作没了"更好解释。
    screen_touch();

    if (in->event == BSP_BTN_LONG) {
        // 这几个手势在配网页打开时也要能用: 上键进演示菜单, 下键按屏幕上的提示
        // 开关配网页。
        if (in->btn == BSP_BTN_DOWN) {
            // 下键是**开关**, 不是"只开不关"。配网页底下写着"长按下键退出配网",
            // 早先这里无条件调 open_provision_window(), 而它已经开着时直接 return ——
            // 用户按屏幕提示去关却关不掉, 整屏被不透明的配网页盖着, 看起来就是死机。
            if (pet_provision_active()) {
                ESP_LOGI(TAG, "长按下键: 退出蓝牙配网");
                (void)pet_provision_stop();
            } else {
                ESP_LOGI(TAG, "长按下键: 打开蓝牙配网");
                open_provision_window();
            }
            return;
        }
        if (in->btn == BSP_BTN_UP) {
            run_demo_menu();
            return;
        }
        if (in->btn == BSP_BTN_OK && !pet_provision_active()) {
            // 开关, 不是"只开不关": 屏幕上的提示写着"长按确定键关闭", 这里若
            // 无条件 set true, 面板开着时长按就是无操作 —— 提示成了假话。和
            // "长按下键开关配网页"保持同一个模式。
            pet_ui_set_info_visible(!pet_ui_info_visible());
        }
        return;
    }

    // 配网页是不透明的, 盖住了整个画面: 此刻其余按键都看不到效果, 直接忽略。
    // 否则会留下"关掉配网页之后信息面板莫名其妙开着"这类状态。
    // 配网页也不能被锁定(配对码必须看得见), 走到这里就顺带拦掉了。
    if (pet_provision_active()) return;

    // 双击确定 = 手动锁定息屏。锁定的价值恰恰是"再来消息也不亮": 挂机、半夜
    // 推送都压得住, 按任意键才回来。bsp 的双击在两次 CLICK 之后到达, 前两下
    // 戳出的互动动画已经播了, 无伤大雅。
    if (in->event == BSP_BTN_DOUBLE) {
        if (in->btn == BSP_BTN_OK) {
            ESP_LOGI(TAG, "双击确定: 手动锁定息屏");
            pet_ui_set_info_visible(false);
            screen_set_user_lock(true);
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
    // 宠物先就位, 界面才知道该按多大的舞台摆版式。槽是空的不是错误: 界面会显示
    // "还没有宠物", 菜单栏连上来看到 hello 里的 pet=none 会自己推一只下来。
    (void)pet_slot_init();
    (void)pet_atlas_load();

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

    // 屏幕从"亮着、开始在计时"起步。此刻还没有链路, 所以亮度按睡眠档给 —— 与
    // pet_ui 的初值(OFFLINE, 宠物在睡觉)一致; 桥接连上会立刻把它顶到全亮。
    pet_screen_init(&s_screen, esp_timer_get_time());
    apply_screen();

    const esp_timer_create_args_t screen_timer_args = {
        .callback = screen_tick,
        .name = "pet_screen",
    };
    if (esp_timer_create(&screen_timer_args, &s_screen_timer) != ESP_OK ||
        esp_timer_start_periodic(s_screen_timer, 1000000) != ESP_OK) {
        ESP_LOGW(TAG, "屏幕定时器没起来, 将不自动息屏(其余功能不受影响)");
    }

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

    // 有可用凭据就照旧直连; 只有实际生效的 SSID 仍是编译期占位符(说明这台设备
    // 还没配过网)时才进蓝牙配网 —— 所以 pet_config_local.h 里填了真实 SSID 的
    // 老用法完全不受影响, 只是多了一个长按下键的配网入口。
    if (s_settings_ok && pet_settings_is_configured(&s_settings)) {
        const esp_err_t bridge_err = start_bridge();
        if (bridge_err != ESP_OK) {
            // 亮度不用在这里给: 链路起不来, s_link_offline 保持 true,
            // 上面 apply_screen() 已经把它压到睡眠档了。直接写这一路会绕开
            // s_backlight_pct 这份缓存 —— 值和缓存一旦分叉, apply_screen()
            // 会因为"值没变"而拒绝重写, 屏上亮度就再也纠正不回来。
            ESP_LOGE(TAG, "Pet Bridge 启动失败: %s; 宠物保持睡眠状态",
                     esp_err_to_name(bridge_err));
        }
    } else {
        ESP_LOGI(TAG, "没有可用的 Wi-Fi 参数, 进入蓝牙配网等 Mac 下发");
        open_provision_window();
    }

    ESP_LOGI(TAG, "就绪。上/下=互动, 确定=戳一下, 长按确定=信息, 双击确定=息屏锁定, "
                  "长按上=演示菜单, 长按下=蓝牙配网");
    return ESP_OK;
}
